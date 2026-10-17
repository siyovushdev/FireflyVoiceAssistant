#include "firefly/admin/LocalMqttBrokerControl.h"
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace firefly::admin {
namespace {
constexpr int ConnectTimeoutMs = 2500;
constexpr int StatusTimeoutMs = 13000;    // two bounded systemctl commands
constexpr int MutationTimeoutMs = 28500; // precheck, change, postcheck
[[noreturn]] void unavailable() {
    throw std::runtime_error("mqtt_broker_control_unavailable");
}
struct Socket {
    int fd = -1;
    ~Socket() { if (fd >= 0) (void)::close(fd); }
};
bool ready(int fd, short events, int timeout) {
    pollfd pfd{fd, events, 0};
    for (;;) {
        const int result = ::poll(&pfd, 1, timeout);
        if (result < 0 && errno == EINTR) continue;
        return result > 0 && (pfd.revents & events) != 0 &&
               (pfd.revents & POLLNVAL) == 0;
    }
}
std::string exchange(int fd, std::string_view request, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    auto remaining = [&]() {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) unavailable();
        return static_cast<int>(left);
    };
    std::size_t sent = 0;
    while (sent < request.size()) {
        if (!ready(fd, POLLOUT, remaining())) unavailable();
        const auto n = ::send(fd, request.data() + sent, request.size() - sent,
                              MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) unavailable();
        sent += static_cast<std::size_t>(n);
    }
    // Frame exactly one operation by ending the write half. The privileged
    // server must examine the *entire* request up to EOF, so an injected
    // second line cannot be smuggled after a valid ENABLE prefix.
    if (::shutdown(fd, SHUT_WR)) unavailable();
    std::string reply;
    reply.reserve(72);
    while (reply.find('\n') == std::string::npos) {
        if (!ready(fd, POLLIN, remaining())) unavailable();
        std::array<char, 80> bytes{};
        const auto n = ::recv(fd, bytes.data(), bytes.size(), MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0 || reply.size() + static_cast<std::size_t>(n) > 96) unavailable();
        reply.append(bytes.data(), static_cast<std::size_t>(n));
    }
    if (reply.back() != '\n' || reply.find('\n') != reply.size()-1) unavailable();
    return reply;
}
} // namespace

LocalMqttBrokerControl::LocalMqttBrokerControl(std::string socketPath,
                                               unsigned trustedPeerUid)
    : socketPath_(std::move(socketPath)), trustedPeerUid_(trustedPeerUid) {}

LocalMqttBrokerStatus LocalMqttBrokerControl::request(const char* verb) const {
    // Every authorized request is a fixed compile-time verb. There are no
    // server-provided or user-controlled executable names, unit names or args.
    if (socketPath_.empty() || socketPath_.size() >= sizeof(sockaddr_un::sun_path))
        unavailable();
    Socket sock;
    sock.fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sock.fd < 0) unavailable();
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socketPath_.c_str(), socketPath_.size()+1);
    if (::connect(sock.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        if (errno != EINPROGRESS || !ready(sock.fd, POLLOUT, ConnectTimeoutMs)) unavailable();
        int result = 0;
        socklen_t size = sizeof(result);
        if (::getsockopt(sock.fd, SOL_SOCKET, SO_ERROR, &result, &size) || result)
            unavailable();
    }
    struct ucred peer{};
    socklen_t len = sizeof(peer);
    if (::getsockopt(sock.fd, SOL_SOCKET, SO_PEERCRED, &peer, &len) ||
        len != sizeof(peer) || peer.uid != trustedPeerUid_)
        unavailable();
    const auto reply = exchange(sock.fd, std::string(verb) + "\n",
                                std::strcmp(verb, "STATUS") == 0 ?
                                    StatusTimeoutMs : MutationTimeoutMs);
    if (reply == "STATE missing disabled stopped\n")
        return {false, false, false};
    if (reply == "STATE present enabled running\n")
        return {true, true, true};
    if (reply == "STATE present enabled stopped\n")
        return {true, true, false};
    if (reply == "STATE present disabled running\n")
        return {true, false, true};
    if (reply == "STATE present disabled stopped\n")
        return {true, false, false};
    if (reply == "ERROR broker_not_installed\n")
        throw std::runtime_error("mqtt_broker_not_installed");
    if (reply == "ERROR broker_control_failed\n")
        throw std::runtime_error("mqtt_broker_control_failed");
    if (reply == "ERROR broker_invalid_state\n")
        throw std::runtime_error("mqtt_broker_invalid_state");
    unavailable();
}
LocalMqttBrokerStatus LocalMqttBrokerControl::status() const {
    return request("STATUS");
}
LocalMqttBrokerStatus LocalMqttBrokerControl::enable() const {
    return request("ENABLE");
}
LocalMqttBrokerStatus LocalMqttBrokerControl::disable() const {
    return request("DISABLE");
}
} // namespace firefly::admin
