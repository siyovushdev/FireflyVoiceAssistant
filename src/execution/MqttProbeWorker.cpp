// Private, same-UID, short-lived executable for the Web Admin MQTT probe.
// Parent supervises its complete lifetime and kills it at the global deadline.
// No shell, privileged identity, arbitrary unit name or secret in argv/env.
#include "firefly/execution/MqttConnectionProbe.h"
#include <chrono>
#include <cerrno>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
bool validLogin(const std::string& input) {
    if (input.size() > 256) return false;
    for (unsigned char ch : input)
        if (ch < 32 || ch == 127) return false;
    return true;
}
std::string readRequest() {
    std::string result;
    char buffer[256];
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(12);
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) throw std::runtime_error("read_timeout");
        pollfd p{STDIN_FILENO, POLLIN, 0};
        const auto ready = ::poll(&p, 1, static_cast<int>(remaining));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || p.revents & (POLLERR | POLLNVAL))
            throw std::runtime_error("read_failed");
        const auto n = ::recv(STDIN_FILENO, buffer, sizeof(buffer), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) throw std::runtime_error("read_failed");
        if (n == 0) break;
        result.append(buffer, static_cast<std::size_t>(n));
        if (result.size() > 4096) throw std::runtime_error("request_too_large");
    }
    return result;
}
void sendResponse(const firefly::execution::MqttProbeResult& result) {
    const nlohmann::json response{
        {"success", result.success}, {"code", result.code},
        {"tls_verified", result.tlsVerified},
        {"credentials_accepted", result.credentialsAccepted}
    };
    const auto body = response.dump();
    if (body.size() > 512) return;
    std::size_t written = 0;
    while (written < body.size()) {
        const auto n = ::send(STDIN_FILENO, body.data() + written,
                              body.size() - written, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        written += static_cast<std::size_t>(n);
    }
    (void)::shutdown(STDIN_FILENO, SHUT_WR);
}
} // namespace

int main(int argc, char** argv) {
    // Any command-line option is invalid. Connection and credentials come only
    // from the private inherited AF_UNIX descriptor, not process listings.
    if (argc != 1 || !argv || !argv[0]) return 2;
    struct rlimit noCore{0, 0};
    if (::setrlimit(RLIMIT_CORE, &noCore) != 0) return 2;
    if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || ::getppid() == 1) return 2;
    struct stat st{};
    if (::fstat(STDIN_FILENO, &st) || !S_ISSOCK(st.st_mode)) return 2;
    try {
        const auto payload = readRequest();
        const auto data = nlohmann::json::parse(payload, nullptr, false);
        if (!data.is_object() || data.size() != 6 ||
            !data.contains("host") || !data.contains("port") ||
            !data.contains("timeout_ms") || !data.contains("tls") ||
            !data.contains("username") || !data.contains("password"))
            return 2;
        if (!data.at("host").is_string() || !data.at("port").is_number_integer() ||
            !data.at("timeout_ms").is_number_integer() ||
            !data.at("tls").is_boolean() || !data.at("username").is_string() ||
            !data.at("password").is_string()) return 2;
        const auto host = data.at("host").get<std::string>();
        const auto user = data.at("username").get<std::string>();
        const auto password = data.at("password").get<std::string>();
        if (host.empty() || host.size() > 253 || !validLogin(user) ||
            !validLogin(password) || user.empty() != password.empty()) return 2;
        firefly::config::Connection connection;
        connection.enabled = true;
        connection.type = firefly::config::ExecutorKind::Mqtt;
        connection.network.host = host;
        connection.network.port = data.at("port").get<std::int64_t>();
        connection.timeoutMs = data.at("timeout_ms").get<std::int64_t>();
        connection.mqtt.tls = data.at("tls").get<bool>();
        sendResponse(firefly::execution::detail::runMqttProbeWorker(
            connection, {user, password}));
        return 0;
    } catch (const std::exception&) {
        // Never serialize the untrusted request, host, or plaintext password.
        return 2;
    }
}
