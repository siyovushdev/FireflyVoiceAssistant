#include "firefly/execution/HttpHardwareExecutor.h"
#include "firefly/actions/ScalarJson.h"
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <netdb.h>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <sstream>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

namespace firefly::execution {
namespace {
struct ParsedUrl {
    std::string host, path = "/";
    int port = 80;
};

bool parseUrl(const std::string& url, ParsedUrl& out) {
    static constexpr std::string_view prefix = "http://";
    if (url.rfind(prefix.data(), 0) != 0) return false;
    auto rest = url.substr(prefix.size());
    if (rest.empty() || rest.find('@') != std::string::npos) return false;
    const auto slash = rest.find('/');
    auto authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    out.path = slash == std::string::npos ? "/" : rest.substr(slash);
    const auto colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(':') == colon) {
        out.host = authority.substr(0, colon);
        try { out.port = std::stoi(authority.substr(colon + 1)); }
        catch (...) { return false; }
        if (out.port <= 0 || out.port > 65535) return false;
    } else out.host = std::move(authority);
    return !out.host.empty() && !out.path.empty() && out.path.front() == '/';
}

std::string buildBody(const config::HttpBinding& http, const actions::GenericAction& action, HardwareResult& error) {
    nlohmann::json body = nlohmann::json::object();
    for (const auto& [key, value] : http.body) body[key] = actions::scalarJson(value);
    if (http.argument) {
        const auto found = action.arguments().find(*http.argument);
        if (found == action.arguments().end()) {
            error = {false, "malformed_binding", {}, ReachabilityEvidence::None};
            return {};
        }
        body[*http.argument] = actions::scalarJson(found->second);
    }
    return body.dump();
}

bool waitWritable(int fd, std::chrono::steady_clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) return false;
    pollfd descriptor{fd, POLLOUT, 0};
    return ::poll(&descriptor, 1, static_cast<int>(left)) > 0 && (descriptor.revents & POLLOUT);
}

bool waitReadable(int fd, std::chrono::steady_clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) return false;
    pollfd descriptor{fd, POLLIN, 0};
    return ::poll(&descriptor, 1, static_cast<int>(left)) > 0 && (descriptor.revents & POLLIN);
}

HardwareResult request(const ParsedUrl& url, const config::HttpBinding& http, const std::string& body) {
    addrinfo hints{}, *list = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    // M14 bindings are numeric IPv4 only (validated). NUMERICHOST refuses DNS so
    // getaddrinfo cannot block beyond a local parse of the address string.
    hints.ai_flags = AI_NUMERICHOST;
    const auto port = std::to_string(url.port);
    if (::getaddrinfo(url.host.c_str(), port.c_str(), &hints, &list) != 0 || !list)
        return {false, "connection_failure", {}, ReachabilityEvidence::Unreachable};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(http.timeoutMs);
    int fd = -1;
    for (auto* item = list; item; item = item->ai_next) {
        fd = ::socket(item->ai_family, item->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, item->ai_protocol);
        if (fd < 0) continue;
        const auto connected = ::connect(fd, item->ai_addr, item->ai_addrlen);
        if (connected == 0) break;
        if (errno == EINPROGRESS && waitWritable(fd, deadline)) {
            int error = 0; socklen_t length = sizeof(error);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0) break;
        }
        ::close(fd); fd = -1;
    }
    ::freeaddrinfo(list);
    if (fd < 0) {
        const auto code = std::chrono::steady_clock::now() >= deadline ? "timeout" : "connection_failure";
        return {false, code, {}, ReachabilityEvidence::Unreachable};
    }

    std::ostringstream message;
    message << http.method << ' ' << url.path << " HTTP/1.1\r\n"
            << "Host: " << url.host << (url.port == 80 ? "" : ":" + port) << "\r\n"
            << "Content-Type: " << http.contentType << "\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n\r\n" << body;
    const auto payload = message.str();
    std::size_t sent = 0;
    while (sent < payload.size()) {
        if (!waitWritable(fd, deadline)) {
            ::close(fd);
            return {false, "timeout", {}, ReachabilityEvidence::Unreachable};
        }
        const auto count = ::send(fd, payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            ::close(fd);
            return {false, "connection_failure", {}, ReachabilityEvidence::Unreachable};
        }
        sent += static_cast<std::size_t>(count);
    }

    std::string response;
    while (response.size() < 65536) {
        if (!waitReadable(fd, deadline)) {
            ::close(fd);
            return {false, "timeout", {}, ReachabilityEvidence::Unreachable};
        }
        char buffer[4096];
        const auto count = ::recv(fd, buffer, sizeof(buffer), 0);
        if (count < 0 && errno == EINTR) continue;
        if (count == 0) break;
        if (count < 0) {
            ::close(fd);
            return {false, "connection_failure", {}, ReachabilityEvidence::Unreachable};
        }
        response.append(buffer, static_cast<std::size_t>(count));
        if (response.find("\r\n\r\n") != std::string::npos && response.size() > 16) {
            // Enough of the status line for a bounded response.
            if (response.find("\r\n") != std::string::npos) break;
        }
    }
    ::close(fd);
    // Unparseable payload is not valid HTTP evidence → no connectivity claim.
    if (response.rfind("HTTP/1.", 0) != 0) return {false, "http_error", {}, ReachabilityEvidence::None};
    const auto space = response.find(' ');
    if (space == std::string::npos) return {false, "http_error", {}, ReachabilityEvidence::None};
    int status = 0;
    try { status = std::stoi(response.substr(space + 1, 3)); }
    catch (...) { return {false, "http_error", {}, ReachabilityEvidence::None}; }
    // Any valid HTTP status proves the transport endpoint was reached.
    if (status < 200 || status > 299)
        return {false, "http_error", std::to_string(status), ReachabilityEvidence::Reachable};
    return {true, {}, std::to_string(status), ReachabilityEvidence::Reachable};
}
}

HardwareResult HttpHardwareExecutor::execute(const config::Binding& binding, const actions::GenericAction& action) {
    if (binding.executor != config::ExecutorKind::Http)
        return {false, "unsupported_executor", {}, ReachabilityEvidence::None};
    ParsedUrl url;
    if (!parseUrl(binding.http.url, url))
        return {false, "malformed_binding", {}, ReachabilityEvidence::None};
    HardwareResult error;
    const auto body = buildBody(binding.http, action, error);
    if (!error.success && !error.error.empty()) return error;
    return request(url, binding.http, body);
}
}
