#include "firefly/execution/HttpHardwareExecutor.h"
#include "firefly/config/Connectivity.h"
#include "firefly/actions/ScalarJson.h"
#include <arpa/inet.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

namespace firefly::execution {
namespace {
constexpr std::size_t MaxBody = 16384, MaxResponse = 65536, MaxHeaders = 8192;
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
struct Socket {
    int fd = -1;
    ~Socket() { if (fd >= 0) ::close(fd); }
    Socket() = default;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
};
struct Endpoint { std::string host, path; int port = 80; };
Endpoint endpoint(const std::string& url) {
    const auto slash = url.find('/', 7);
    const auto authority = url.substr(7, slash == std::string::npos ? slash : slash - 7);
    const auto colon = authority.find(':');
    return {authority.substr(0, colon), slash == std::string::npos ? "/" : url.substr(slash),
        colon == std::string::npos ? 80 : std::stoi(authority.substr(colon + 1))};
}
HardwareResult fail(const std::string& code, ReachabilityEvidence reach = ReachabilityEvidence::None) {
    return {false, code, {}, reach};
}
bool wait(int fd, short events, Clock::time_point deadline) {
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) return false;
        pollfd descriptor{fd, events, 0};
        const auto result = ::poll(&descriptor, 1, static_cast<int>(left));
        if (result < 0 && errno == EINTR) continue;
        return result > 0 && (descriptor.revents & (events | POLLHUP | POLLERR));
    }
}
HardwareResult connect(Socket& socket, const Endpoint& url, Clock::time_point deadline) {
    socket.fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (socket.fd < 0) return fail("connection_failure", ReachabilityEvidence::Unreachable);
    sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_port = htons(static_cast<unsigned short>(url.port));
    if (::inet_pton(AF_INET, url.host.c_str(), &address.sin_addr) != 1) return fail("malformed_binding");
    if (::connect(socket.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        if (errno != EINPROGRESS) return fail("connection_failure", ReachabilityEvidence::Unreachable);
        if (!wait(socket.fd, POLLOUT, deadline)) return fail("timeout", ReachabilityEvidence::Unreachable);
        int error = 0; socklen_t size = sizeof(error);
        if (::getsockopt(socket.fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error)
            return fail("connection_failure", ReachabilityEvidence::Unreachable);
    }
    return {true, {}, {}, ReachabilityEvidence::Reachable};
}
bool number(const std::string& text, std::size_t& result, int base = 10) {
    if (text.empty()) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result, base);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
std::string lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
// Parse full framing before reporting success, including fragmented/chunked bodies.
struct Response {
    int status = 0;
    std::string body, error;
    bool done = false;
};
Response parseResponse(const std::string& wire, bool eof) {
    Response r;
    const auto end = wire.find("\r\n\r\n");
    if (end == std::string::npos) {
        if (wire.size() > MaxHeaders) r.error = "response_limit";
        else if (eof) r.error = "malformed_response";
        return r;
    }
    if (end > MaxHeaders) { r.error = "response_limit"; return r; }
    const auto firstEnd = wire.find("\r\n");
    const auto first = wire.substr(0, firstEnd);
    std::size_t code = 0;
    if (first.size() < 13 || (first.rfind("HTTP/1.1 ", 0) != 0 && first.rfind("HTTP/1.0 ", 0) != 0) ||
        first[12] != ' ' || !number(first.substr(9, 3), code) || code < 100 || code > 599) {
        r.error = "malformed_response"; return r;
    }
    r.status = static_cast<int>(code);
    std::map<std::string, std::string> headers;
    for (auto pos = firstEnd + 2; pos < end;) {
        const auto next = wire.find("\r\n", pos);
        const auto colon = wire.find(':', pos);
        if (colon == std::string::npos || colon >= next || colon == pos) { r.error = "malformed_response"; return r; }
        auto value = wire.substr(colon + 1, next - colon - 1);
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
        while (!value.empty() && value.back() == ' ') value.pop_back();
        const auto key = lower(wire.substr(pos, colon - pos));
        if (!headers.emplace(key, value).second && (key == "content-length" || key == "transfer-encoding")) {
            r.error = "malformed_response"; return r;
        }
        pos = next + 2;
    }
    const auto payload = wire.substr(end + 4);
    if (payload.size() > MaxResponse) { r.error = "response_limit"; return r; }
    if (headers.count("content-encoding") && lower(headers.at("content-encoding")) != "identity") { r.error = "malformed_response"; return r; }
    if (headers.count("transfer-encoding")) {
        if (headers.count("content-length") || lower(headers.at("transfer-encoding")) != "chunked") { r.error = "malformed_response"; return r; }
        std::size_t pos = 0;
        for (;;) {
            const auto line = payload.find("\r\n", pos);
            if (line == std::string::npos) break;
            std::size_t size = 0;
            const auto sizeText = payload.substr(pos, line - pos);
            // Chunk extensions/trailers intentionally unsupported, fail closed.
            if (!number(sizeText, size, 16)) { r.error = "malformed_response"; return r; }
            if (size > MaxResponse || r.body.size() + size > MaxResponse) { r.error = "response_limit"; return r; }
            pos = line + 2;
            if (payload.size() - pos < size + 2) break;
            if (payload.substr(pos + size, 2) != "\r\n") { r.error = "malformed_response"; return r; }
            r.body.append(payload, pos, size); pos += size + 2;
            if (size == 0) { r.done = pos == payload.size(); if (!r.done) r.error = "malformed_response"; return r; }
        }
    } else if (headers.count("content-length")) {
        std::size_t size = 0;
        if (!number(headers.at("content-length"), size)) { r.error = "malformed_response"; return r; }
        if (size > MaxResponse) { r.error = "response_limit"; return r; }
        if (payload.size() > size) { r.error = "malformed_response"; return r; }
        if (payload.size() == size) { r.body = payload; r.done = true; return r; }
    } else if (r.status == 204) {
        r.done = payload.empty(); if (!r.done) r.error = "malformed_response"; return r;
    } else if (eof) { r.body = payload; r.done = true; return r; }
    if (eof) r.error = "malformed_response";
    return r;
}
struct Reply { HardwareResult result; std::string body; };
Reply request(const config::HttpBinding& http, const std::string& body) {
    if (body.size() > MaxBody) return {fail("request_limit"), {}};
    const auto url = endpoint(http.url);
    const auto deadline = Clock::now() + std::chrono::milliseconds(http.timeoutMs);
    Socket socket;
    const auto connected = connect(socket, url, deadline);
    if (!connected.success) return {connected, {}};
    std::ostringstream message;
    message << http.method << ' ' << url.path << " HTTP/1.1\r\nHost: " << url.host << ':' << url.port
            << "\r\nContent-Type: " << http.contentType << "\r\nContent-Length: " << body.size() << "\r\nConnection: close\r\n";
    for (const auto& [name, value] : http.headers) message << name << ": " << value << "\r\n";
    message << "\r\n" << body;
    const auto payload = message.str();
    std::size_t sent = 0;
    while (sent < payload.size()) {
        if (!wait(socket.fd, POLLOUT, deadline)) return {fail("timeout", ReachabilityEvidence::Unreachable), {}};
        const auto count = ::send(socket.fd, payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count <= 0) return {fail("connection_failure", ReachabilityEvidence::Unreachable), {}};
        sent += static_cast<std::size_t>(count);
    }
    std::string wire;
    for (;;) {
        if (!wait(socket.fd, POLLIN, deadline)) return {fail("timeout", ReachabilityEvidence::Unreachable), {}};
        char buffer[4096];
        const auto count = ::recv(socket.fd, buffer, sizeof(buffer), 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count < 0) return {fail("connection_failure", ReachabilityEvidence::Unreachable), {}};
        wire.append(buffer, static_cast<std::size_t>(count));
        if (wire.size() > MaxResponse + MaxHeaders) return {fail("response_limit"), {}};
        const auto response = parseResponse(wire, count == 0);
        const auto reach = response.status ? ReachabilityEvidence::Reachable : ReachabilityEvidence::None;
        if (!response.error.empty()) return {fail(response.error, reach), {}};
        if (!response.done) continue;
        if (response.status < 200 || response.status > 299 || (http.expectedStatus && response.status != http.expectedStatus))
            return {{false, "http_error", std::to_string(response.status), reach}, {}};
        return {{true, {}, std::to_string(response.status), reach}, response.body};
    }
}
}
HardwareResult HttpHardwareExecutor::prepare(const std::string& id, const config::HttpBinding& mapping,
                                            config::HttpBinding& effective, bool observe) const {
    const auto* connection = catalog_ ? config::findConnection(catalog_->configuration(), id) : nullptr;
    if (!id.empty() && !connection) return fail("connection_missing");
    if (connection && !connection->enabled) return fail("connection_disabled");
    try {
        config::validateHttpMapping(mapping, connection, observe);
        effective = config::effectiveHttp(mapping, connection);
    } catch (const config::ConfigError&) { return fail("malformed_binding"); }
    return {true, {}, {}, ReachabilityEvidence::None};
}
HardwareResult HttpHardwareExecutor::execute(const config::Binding& binding, const actions::GenericAction& action) {
    if (binding.executor != config::ExecutorKind::Http) return fail("unsupported_executor");
    if (binding.targetId != action.targetId() || binding.actionId != action.actionId()) return fail("malformed_binding");
    config::HttpBinding http;
    const auto ready = prepare(binding.connectionId, binding.http, http);
    if (!ready.success) return ready;
    Json body = Json::object();
    for (const auto& [key, value] : http.body) body[key] = actions::scalarJson(value);
    if (http.argument) {
        const auto found = action.arguments().find(*http.argument);
        if (found == action.arguments().end()) return fail("malformed_binding");
        body[*http.argument] = actions::scalarJson(found->second);
    }
    return request(http, http.method == "GET" ? "" : body.dump()).result;
}
HardwareObservation HttpHardwareExecutor::observe(const config::ObservationBinding& binding) {
    config::HttpBinding http;
    const auto ready = prepare(binding.connectionId, binding.http, http, true);
    if (!ready.success) return {ready, {}};
    const auto reply = request(http, "");
    if (!reply.result.success) return {reply.result, {}};
    try {
        actions::checkNumericLexemes(reply.body);
        std::map<int, std::set<std::string>> keys;
        const auto body = Json::parse(reply.body, [&](int depth, Json::parse_event_t event, Json& item) {
            if (depth > 16) throw std::runtime_error("depth");
            if (event == Json::parse_event_t::object_start) keys[depth] = {};
            if (event == Json::parse_event_t::key && !keys.at(depth - 1).insert(item.get<std::string>()).second) throw std::runtime_error("duplicate");
            return true;
        });
        if (!body.is_object()) throw std::runtime_error("object required");
        HardwareObservation result{reply.result, {}};
        for (const auto& [source, field] : binding.fields) result.properties.emplace(field, actions::decodeScalar(body.at(source)));
        return result;
    } catch (const std::exception&) { return {fail("malformed_observation", ReachabilityEvidence::Reachable), {}}; }
}
HardwareResult HttpHardwareExecutor::testConnection(const config::Connection& connection) {
    if (!connection.enabled) return fail("connection_disabled");
    if (connection.type != config::ExecutorKind::Http) return fail("unsupported_executor");
    try {
        config::Configuration candidate; candidate.schemaVersion = 2; candidate.connections.push_back(connection);
        config::validateConnections(candidate);
        Socket socket;
        return connect(socket, endpoint(connection.http.baseUrl), Clock::now() + std::chrono::milliseconds(connection.timeoutMs));
    } catch (const config::ConfigError&) { return fail("malformed_binding"); }
}
}
