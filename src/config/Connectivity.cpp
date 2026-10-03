#include "firefly/config/Connectivity.h"
#include "firefly/actions/ScalarJson.h"
#include <arpa/inet.h>
#include <algorithm>
#include <cctype>

namespace firefly::config {
namespace {
[[noreturn]] void invalid(const std::string& message) {
    throw ConfigError("malformed_binding", "/connections", message);
}
bool clean(const std::string& text) {
    return std::none_of(text.begin(), text.end(), [](unsigned char c) { return c <= 32 || c == 127; });
}
void path(const std::string& value, bool base = false) {
    if (base && value.empty()) return;
    if (value.empty() || value.front() != '/' || value.rfind("//", 0) == 0 || !clean(value) ||
        value.find_first_of("#\\{}$") != std::string::npos || value.size() > 512 ||
        (base && value.find('?') != std::string::npos)) invalid("Path must be a literal absolute path without fragments or templates");
}
void url(const std::string& value, bool base) {
    if (value.rfind("http://", 0) != 0 || !clean(value) || value.size() > 1024 ||
        value.find_first_of("@#\\{}$") != std::string::npos) invalid("Only http://numeric-IPv4[:port] is supported; credentials, TLS and templates are not supported");
    const auto slash = value.find('/', 7);
    const auto authority = value.substr(7, slash == std::string::npos ? slash : slash - 7);
    const auto colon = authority.find(':');
    const auto host = authority.substr(0, colon);
    in_addr address{};
    if (::inet_pton(AF_INET, host.c_str(), &address) != 1) invalid("Numeric IPv4 required; DNS is not supported");
    if (colon != std::string::npos) {
        const auto port = authority.substr(colon + 1);
        if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos ||
            std::stoi(port) < 1 || std::stoi(port) > 65535) invalid("Invalid TCP port");
    }
    if (slash != std::string::npos) {
        if (base && value.substr(slash) != "/") invalid("Use base_path separately from base_url");
        path(value.substr(slash));
    }
}
}
std::string executorName(ExecutorKind kind) {
    switch (kind) {
        case ExecutorKind::Http: return "http";
        case ExecutorKind::Mqtt: return "mqtt";
        case ExecutorKind::Tcp: return "tcp";
        case ExecutorKind::Udp: return "udp";
        case ExecutorKind::Serial: return "serial";
        case ExecutorKind::Can: return "can";
        default: throw ConfigError("unsupported_executor", "/connections/type", "Unknown transport");
    }
}
ExecutorKind executorKind(const std::string& name) {
    for (auto kind : {ExecutorKind::Http, ExecutorKind::Mqtt, ExecutorKind::Tcp, ExecutorKind::Udp, ExecutorKind::Serial, ExecutorKind::Can})
        if (executorName(kind) == name) return kind;
    return ExecutorKind::Unknown;
}
const Connection* findConnection(const Configuration& config, const std::string& id) {
    for (const auto& connection : config.connections) if (connection.id == id) return &connection;
    return nullptr;
}
void validateConnections(const Configuration& config) {
    if (config.schemaVersion != 2 && (!config.connections.empty() || !config.observations.empty()))
        throw ConfigError("unsupported_version", "/connections", "Connections require v2");
    for (std::size_t i = 0; i < config.connections.size(); ++i) {
        const auto& c = config.connections[i];
        std::string field;
        try {
            field = "/timeout_ms";
            if (c.timeoutMs < 50 || c.timeoutMs > 10000) invalid("Timeout must be 50..10000 ms");
            if (c.type == ExecutorKind::Http) { field = "/http/base_url"; url(c.http.baseUrl, true); field = "/http/base_path"; path(c.http.basePath, true); }
            else {
                field = "/type";
                if (c.enabled) throw ConfigError("unsupported_executor", "/connections", "Planned transports must remain disabled");
                if (c.type == ExecutorKind::Mqtt || c.type == ExecutorKind::Tcp || c.type == ExecutorKind::Udp) {
                    if (c.network.host.empty() || !clean(c.network.host) || c.network.port < 1 || c.network.port > 65535) invalid("Network endpoint required");
                } else if (c.type == ExecutorKind::Serial) {
                    if (c.serial.port.rfind("/dev/", 0) != 0 || !clean(c.serial.port) || c.serial.baudRate < 50 || c.serial.baudRate > 4000000) invalid("Serial port and baud rate required");
                } else if (c.type == ExecutorKind::Can) {
                    if (c.can.interface.empty() || !clean(c.can.interface)) invalid("CAN interface required");
                } else invalid("Unknown transport type");
            }
        } catch (const ConfigError& error) {
            throw ConfigError(error.code, "/connections/" + std::to_string(i) + field, error.what());
        }
    }
}
HttpBinding effectiveHttp(const HttpBinding& mapping, const Connection* connection) {
    auto result = mapping;
    if (connection) {
        auto base = connection->http.baseUrl;
        if (!base.empty() && base.back() == '/') base.pop_back();
        auto prefix = connection->http.basePath;
        if (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
        result.url = base + prefix + mapping.path;
        if (result.timeoutMs == 0) result.timeoutMs = connection->timeoutMs;
    }
    return result;
}
void validateHttpMapping(const HttpBinding& h, const Connection* c, bool observe) {
    if (c) {
        if (c->type != ExecutorKind::Http) invalid("Mapping transport differs from connection type");
        if (!h.url.empty()) invalid("Connection bindings use path, never url");
        path(h.path);
    } else {
        if (!h.path.empty()) invalid("A path requires connection_id");
        url(h.url, false);
    }
    if (observe) {
        if (h.method != "GET" || !h.body.empty() || h.argument) invalid("Observe requires GET without body or arguments");
    } else if (h.method != "POST" && h.method != "PUT" && h.method != "PATCH" && h.method != "DELETE" && h.method != "GET") invalid("Unsupported HTTP method");
    if (h.method == "GET" && (!h.body.empty() || h.argument)) invalid("GET must not carry a body");
    const auto effective = effectiveHttp(h, c);
    url(effective.url, false);
    if (effective.timeoutMs < 50 || effective.timeoutMs > 10000) invalid("Timeout must be 50..10000 ms (0 inherits Connection)");
    if (h.expectedStatus != 0 && (h.expectedStatus < 200 || h.expectedStatus > 299)) invalid("Success status must be 2xx");
    if (h.contentType.empty() || h.contentType.size() > 128 || std::any_of(h.contentType.begin(), h.contentType.end(), [](unsigned char ch) { return ch < 32 || ch == 127; })) invalid("Invalid Content-Type");
    std::set<std::string> names;
    if (h.headers.size() > 16) invalid("Too many headers");
    for (const auto& [name, value] : h.headers) {
        std::string lower;
        for (unsigned char ch : name) lower += static_cast<char>(std::tolower(ch));
        if (name.empty() || name.size() > 64 || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") != std::string::npos ||
            !names.insert(lower).second || (lower != "accept" && lower.rfind("x-", 0) != 0) ||
            lower.find("auth") != std::string::npos || lower.find("key") != std::string::npos || lower.find("token") != std::string::npos ||
            value.size() > 512 || std::any_of(value.begin(), value.end(), [](unsigned char ch) { return ch < 32 || ch == 127; })) invalid("Only non-secret Accept / X-* headers are supported");
    }
    nlohmann::json body = nlohmann::json::object();
    for (const auto& [key, value] : h.body) {
        body[key] = actions::scalarJson(value);
        if (key.empty() || key.size() > 64) invalid("Body keys must be 1..64 bytes");
        if (const auto* text = std::get_if<std::string>(&value.data()))
            if (text->find("${") != std::string::npos || text->find("{{") != std::string::npos) invalid("Templates are not supported; use declared argument");
    }
    if (body.dump().size() > 16384) invalid("Request body exceeds 16 KiB");
}
}
