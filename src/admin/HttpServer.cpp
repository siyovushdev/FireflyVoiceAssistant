#include "HttpServer.h"
#include "AdminV2Assets.h"
#include "Json.h"
#include <arpa/inet.h>
#include <atomic>
#include <charconv>
#include <chrono>
#include <fstream>
#include <map>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace firefly::admin {
namespace {
struct Socket {
    int fd;
    ~Socket() { if (fd >= 0) ::close(fd); }
};
struct HttpError : std::runtime_error {
    int status;
    explicit HttpError(int code, const char* message) : std::runtime_error(message), status(code) {}
};
HttpReply errorReply(int status, const std::string& message) {
    return {status, "application/json; charset=utf-8", failure("http_request", "", message).dump()};
}
void sendReply(int socket, const HttpReply& reply) {
    const std::string header = "HTTP/1.1 " + std::to_string(reply.status) + " Response\r\nContent-Type: " + reply.contentType +
        "\r\nContent-Length: " + std::to_string(reply.body.size()) +
        "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n"
        "Content-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'\r\n\r\n";
    const auto message = header + reply.body;
    std::size_t sent = 0;
    while (sent < message.size()) {
        const auto result = ::send(socket, message.data() + sent, message.size() - sent, MSG_NOSIGNAL);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return;
        sent += static_cast<std::size_t>(result);
    }
}
std::string trim(std::string value) {
    const auto begin = value.find_first_not_of(" \t");
    if (begin == std::string::npos) return {};
    return value.substr(begin, value.find_last_not_of(" \t") - begin + 1);
}
HttpReply request(int socket, Api& api, const std::map<std::string, HttpReply>& assets) {
    std::string bytes;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto receive = [&] {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) throw HttpError(408, "Request timed out.");
        pollfd descriptor{socket, POLLIN, 0};
        if (::poll(&descriptor, 1, static_cast<int>(left)) <= 0) throw HttpError(408, "Request timed out.");
        char buffer[8192];
        const auto count = ::recv(socket, buffer, sizeof(buffer), 0);
        if (count <= 0) throw HttpError(400, "Incomplete HTTP request.");
        bytes.append(buffer, static_cast<std::size_t>(count));
    };
    while (bytes.find("\r\n\r\n") == std::string::npos) {
        if (bytes.size() > 16384) throw HttpError(431, "Headers too large.");
        receive();
    }
    const auto end = bytes.find("\r\n\r\n");
    if (end > 16384) throw HttpError(431, "Headers too large.");
    std::istringstream lines(bytes.substr(0, end));
    std::string line, method, path, version, extra;
    std::getline(lines, line);
    std::istringstream first(line);
    if (!(first >> method >> path >> version) || first >> extra || version != "HTTP/1.1" ||
        path.empty() || path.front() != '/' || path.find_first_of("?#%\\") != std::string::npos)
        throw HttpError(400, "Invalid HTTP request line.");
    std::map<std::string, std::string> headers;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos || colon == 0) throw HttpError(400, "Invalid HTTP header.");
        std::string name = line.substr(0, colon);
        for (char& c : name) {
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && std::string_view("!#$%&'*+-.^_`|~").find(c) == std::string_view::npos)
                throw HttpError(400, "Invalid HTTP header name.");
        }
        std::string value = trim(line.substr(colon + 1));
        for (const unsigned char c : value) if (c < 32 || c == 127) throw HttpError(400, "Invalid HTTP header value.");
        if (!headers.emplace(name, value).second) throw HttpError(400, "Duplicate HTTP header.");
    }
    if (!headers.count("host") || headers["host"].empty()) throw HttpError(400, "Host header required.");
    if (headers.count("transfer-encoding") || headers.count("expect")) throw HttpError(400, "Only fixed-length request bodies are supported.");
    std::size_t length = 0;
    if (headers.count("content-length")) {
        const auto& value = headers["content-length"];
        const auto result = std::from_chars(value.data(), value.data() + value.size(), length);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) throw HttpError(400, "Invalid Content-Length.");
    }
    if (length > (path == "/api/admin/transcribe" ? 60*48000*2+256 : MaxMessageBytes))
        throw HttpError(413, "Request body too large.");
    const bool mutation = method != "GET" && method != "HEAD";
    if (mutation) {
        if (headers["x-firefly-admin"] != "1") throw HttpError(403, "X-Firefly-Admin: 1 is required.");
        if (headers.count("origin") && headers["origin"] != "http://" + headers["host"])
            throw HttpError(403, "Cross-origin mutation rejected.");
        if (headers.count("sec-fetch-site") && headers["sec-fetch-site"] == "cross-site")
            throw HttpError(403, "Cross-site mutation rejected.");
        const auto& type = headers["content-type"];
        if (path == "/api/admin/transcribe") {
            if (type != "audio/wav" && type != "audio/x-wav" && type != "application/octet-stream")
                throw HttpError(415, "Use audio/wav for Admin Copilot transcription.");
        } else if (type != "application/json" && type != "application/json; charset=utf-8")
            throw HttpError(415, "Use application/json.");
    }
    const std::size_t bodyStart = end + 4;
    while (bytes.size() < bodyStart + length) receive();
    if (bytes.size() != bodyStart + length) throw HttpError(400, "Unexpected trailing request bytes.");
    if (path.rfind("/api/", 0) == 0) return api.handle(method, path, bytes.substr(bodyStart), headers["x-firefly-setup"]);
    if (method != "GET") return errorReply(405, "Static resources support GET only.");
    if (length) throw HttpError(400, "Unexpected GET body.");
    const auto asset = assets.find(path);
    return asset == assets.end() ? errorReply(404, "Resource not found.") : asset->second;
}
}
struct HttpServer::Impl {
    Socket listener{-1};
    Api& api;
    unsigned short port = 0;
    std::map<std::string, HttpReply> assets;
    struct Client { std::thread thread; std::shared_ptr<std::atomic<bool>> done; };
    std::vector<Client> clients;
    explicit Impl(Api& api) : api(api) {}
    ~Impl() { for (auto& client : clients) if (client.thread.joinable()) client.thread.join(); }
};
HttpServer::HttpServer(Api& api, const std::string& bindAddress, unsigned short port) : impl_(std::make_unique<Impl>(api)) {
    for (const auto& file : std::map<std::string, std::string>{{"index.html", "text/html; charset=utf-8"}, {"admin.js", "text/javascript; charset=utf-8"}, {"admin-model.js", "text/javascript; charset=utf-8"}, {"typed-editor.js", "text/javascript; charset=utf-8"}, {"simple-editor.js", "text/javascript; charset=utf-8"}, {"connection-editor.js", "text/javascript; charset=utf-8"}, {"admin.css", "text/css; charset=utf-8"}}) {
        std::ifstream stream(std::string(FIREFLY_ADMIN_ASSETS) + "/" + file.first, std::ios::binary);
        if (!stream) throw std::runtime_error("Admin static assets unavailable");
        impl_->assets[file.first == "index.html" ? "/" : "/" + file.first] = {200, file.second, {std::istreambuf_iterator<char>(stream), {}}};
    }
    // Generated, verified allowlist. B1 keeps vanilla at / and V2 at /v2/.
    // Requests never translate their URL into a filesystem path.
    addV2Assets(impl_->assets);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
    if (::inet_pton(AF_INET, bindAddress.c_str(), &address.sin_addr) != 1) throw std::invalid_argument("Admin bind must be an explicit IPv4 address");
    impl_->listener.fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (impl_->listener.fd < 0) throw std::runtime_error("Cannot create admin listener");
    const int reuse = 1;
    ::setsockopt(impl_->listener.fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (::bind(impl_->listener.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || ::listen(impl_->listener.fd, 16))
        throw std::runtime_error("Cannot bind admin address/port");
    socklen_t size = sizeof(address);
    if (::getsockname(impl_->listener.fd, reinterpret_cast<sockaddr*>(&address), &size)) throw std::runtime_error("Cannot inspect admin listener");
    impl_->port = ntohs(address.sin_port);
}
HttpServer::~HttpServer() = default;
unsigned short HttpServer::port() const { return impl_->port; }
void HttpServer::finishClients() {
    impl_->api.beginShutdown();
    for(auto& client:impl_->clients) if(client.thread.joinable())client.thread.join();
    impl_->clients.clear();
}
void HttpServer::run(const volatile std::sig_atomic_t& stopping) {
    while (!stopping) {
        for (auto it = impl_->clients.begin(); it != impl_->clients.end();) {
            if (it->done->load()) { it->thread.join(); it = impl_->clients.erase(it); } else ++it;
        }
        pollfd descriptor{impl_->listener.fd, POLLIN, 0};
        const int count = ::poll(&descriptor, 1, 100);
        if (count <= 0) continue;
        const int fd = ::accept4(impl_->listener.fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) continue;
        timeval timeout{5, 0}; ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (impl_->clients.size() >= 16) { sendReply(fd, errorReply(503, "Too many concurrent requests.")); ::close(fd); continue; }
        const auto done = std::make_shared<std::atomic<bool>>(false);
        impl_->clients.push_back({std::thread([this, fd, done] {
            Socket socket{fd};
            try { sendReply(fd, request(fd, impl_->api, impl_->assets)); }
            catch (const HttpError& error) { sendReply(fd, errorReply(error.status, error.what())); }
            catch (...) { sendReply(fd, errorReply(500, "Request could not be completed.")); }
            *done = true;
        }), done});
    }
    finishClients();
}
}
