#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "firefly/execution/MqttConnectionProbe.h"
#include "MqttLibrary.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <set>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

#ifdef FIREFLY_WITH_MOSQUITTO
#include <mosquitto.h>
#endif
extern char **environ;

namespace firefly::execution {
namespace detail {
#ifdef FIREFLY_WITH_MOSQUITTO
void ensureMqttLibrary() {
    static std::once_flag once;
    static bool initialized = false;
    std::call_once(once, [] { initialized = mosquitto_lib_init() == MOSQ_ERR_SUCCESS; });
    if (!initialized) throw std::runtime_error("mqtt_library_unavailable");
}
#else
void ensureMqttLibrary() { throw std::runtime_error("mqtt_unavailable"); }
#endif
}
namespace {
using Clock = std::chrono::steady_clock;
MqttProbeResult rejected(std::string code) {
    return {false, std::move(code), false, false};
}
bool validConnection(const config::Connection& connection) {
    return connection.type == config::ExecutorKind::Mqtt && connection.enabled &&
           !connection.network.host.empty() && connection.network.port >= 1 &&
           connection.network.port <= 65535 && connection.timeoutMs >= 50 &&
           connection.timeoutMs <= 10000;
}
#ifdef FIREFLY_WITH_MOSQUITTO
// Parent admits one bounded helper at a time, never a detached resolver
// thread. Killing the *process* stops libc getaddrinfo and releases all FDs.
std::atomic<bool> probeInProgress{false};
struct ProbeSlot {
    bool acquired = !probeInProgress.exchange(true, std::memory_order_acq_rel);
    ~ProbeSlot() {
        if (acquired) probeInProgress.store(false, std::memory_order_release);
    }
};
struct Fd {
    int value = -1;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { if (value >= 0) (void)::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value(other.value) { other.value = -1; }
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            if (value >= 0) (void)::close(value);
            value = other.value; other.value = -1;
        }
        return *this;
    }
};
struct SpawnActions {
    posix_spawn_file_actions_t actions{};
    bool valid = false;
    SpawnActions() { valid = posix_spawn_file_actions_init(&actions) == 0; }
    ~SpawnActions() { if (valid) posix_spawn_file_actions_destroy(&actions); }
};
struct Child {
    pid_t pid = -1;
    bool reaped = false;
    explicit Child(pid_t child) : pid(child) {}
    ~Child() {
        if (pid <= 0 || reaped) return;
        (void)::kill(pid, SIGKILL);
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
};
int leftMs(Clock::time_point deadline) {
    const auto now = Clock::now();
    if (now >= deadline) return 0;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now).count();
    return left < 1 ? 1 : left > INT_MAX ? INT_MAX : static_cast<int>(left);
}
bool ready(int fd, short events, Clock::time_point deadline) {
    for (;;) {
        const auto remaining = leftMs(deadline);
        if (remaining <= 0) return false;
        pollfd pfd{fd, events, 0};
        const int n = ::poll(&pfd, 1, remaining);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || pfd.revents & (POLLERR | POLLNVAL)) return false;
        if (pfd.revents & (events | POLLHUP)) return true;
    }
}
bool sendAll(int fd, const std::string& bytes, Clock::time_point deadline) {
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        if (!ready(fd, POLLOUT, deadline)) return false;
        const auto n = ::send(fd, bytes.data() + pos, bytes.size() - pos,
                              MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) return false;
        pos += static_cast<std::size_t>(n);
    }
    return ::shutdown(fd, SHUT_WR) == 0;
}
bool receiveBounded(int fd, std::string& bytes, Clock::time_point deadline) {
    while (bytes.size() <= 512) {
        if (!ready(fd, POLLIN, deadline)) return false;
        char buffer[128];
        const auto n = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n < 0) return false;
        if (n == 0) return !bytes.empty() && bytes.size() <= 512;
        bytes.append(buffer, static_cast<std::size_t>(n));
    }
    return false;
}
bool reapBeforeDeadline(Child& child, Clock::time_point deadline) {
    for (;;) {
        int status = 0;
        const auto pid = ::waitpid(child.pid, &status, WNOHANG);
        if (pid == child.pid) {
            child.reaped = true;
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (pid < 0 && errno != EINTR) return false;
        const auto remaining = leftMs(deadline);
        if (remaining <= 0) return false;
        (void)::poll(nullptr, 0, remaining > 5 ? 5 : remaining);
    }
}
std::string siblingWorkerPath() {
    char path[PATH_MAX + 1]{};
    const auto n = ::readlink("/proc/self/exe", path, PATH_MAX);
    if (n <= 0 || n >= PATH_MAX) throw std::runtime_error("mqtt_probe_worker_unavailable");
    path[n] = 0;
    std::string executable(path);
    const auto separator = executable.find_last_of('/');
    if (separator == std::string::npos) throw std::runtime_error("mqtt_probe_worker_unavailable");
    const auto worker = executable.substr(0, separator + 1) + "firefly-mqtt-probe-worker";
    struct stat st{};
    if (::lstat(worker.c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
        (st.st_uid != ::geteuid() && st.st_uid != 0) ||
        (st.st_mode & 022) != 0 || (st.st_mode & 0111) == 0)
        throw std::runtime_error("mqtt_probe_worker_unavailable");
    return worker;
}
MqttProbeResult subprocessProbe(const config::Connection& connection,
                                const MqttLogin& login, Clock::time_point deadline) {
    const nlohmann::json message = {
        {"host", connection.network.host}, {"port", connection.network.port},
        {"timeout_ms", connection.timeoutMs}, {"tls", connection.mqtt.tls},
        {"username", login.user}, {"password", login.password}
    };
    auto request = message.dump() + "\n";
    if (request.size() > 4096) return rejected("mqtt_probe_configuration");
    if (leftMs(deadline) == 0) return rejected("mqtt_probe_timeout");
    const auto workerPath = siblingWorkerPath();
    int sockets[2]{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets))
        return rejected("mqtt_probe_worker_unavailable");
    Fd parent(sockets[0]), childSocket(sockets[1]);
    // fd 0 must be duplicated from a distinct fd >= 3 so dup2 clears
    // CLOEXEC; the spawned child closes every inherited fd >= 3.
    if (childSocket.value < 3) {
        const int copy = ::fcntl(childSocket.value, F_DUPFD_CLOEXEC, 3);
        if (copy < 0) return rejected("mqtt_probe_worker_unavailable");
        childSocket = Fd(copy);
    }
    SpawnActions actions;
    if (!actions.valid ||
        posix_spawn_file_actions_adddup2(&actions.actions, childSocket.value, STDIN_FILENO) ||
        posix_spawn_file_actions_addopen(&actions.actions, STDOUT_FILENO,
                                         "/dev/null", O_WRONLY, 0600) ||
        posix_spawn_file_actions_addopen(&actions.actions, STDERR_FILENO,
                                         "/dev/null", O_WRONLY, 0600) ||
        posix_spawn_file_actions_addclosefrom_np(&actions.actions, 3))
        return rejected("mqtt_probe_worker_unavailable");
    char* args[]{const_cast<char*>(workerPath.c_str()), nullptr};
    pid_t pid = -1;
    const int spawnStatus = ::posix_spawn(&pid, workerPath.c_str(),
                                        &actions.actions, nullptr, args, environ);
    if (spawnStatus != 0 || pid <= 0)
        return rejected("mqtt_probe_worker_unavailable");
    Child child(pid);
    childSocket = Fd{};
    if (!sendAll(parent.value, request, deadline))
        return rejected(leftMs(deadline) == 0 ? "mqtt_probe_timeout" :
                                                   "mqtt_probe_worker_unavailable");
    std::string response;
    if (!receiveBounded(parent.value, response, deadline))
        return rejected(leftMs(deadline) == 0 ? "mqtt_probe_timeout" :
                                                   "mqtt_probe_worker_unavailable");
    if (!reapBeforeDeadline(child, deadline) || leftMs(deadline) == 0)
        return rejected(leftMs(deadline) == 0 ? "mqtt_probe_timeout" :
                                                   "mqtt_probe_worker_unavailable");
    try {
        const auto parsed = nlohmann::json::parse(response);
        if (!parsed.is_object() ||
            parsed.size() != 4 || !parsed.contains("success") ||
            !parsed.contains("code") || !parsed.contains("tls_verified") ||
            !parsed.contains("credentials_accepted"))
            return rejected("mqtt_probe_worker_unavailable");
        MqttProbeResult result{
            parsed.at("success").get<bool>(),
            parsed.at("code").get<std::string>(),
            parsed.at("tls_verified").get<bool>(),
            parsed.at("credentials_accepted").get<bool>()
        };
        static const std::set<std::string> codes{
            "", "mqtt_probe_timeout", "mqtt_probe_connection_failed",
            "mqtt_probe_connack_rejected", "mqtt_probe_auth_rejected",
            "mqtt_probe_tls_configuration", "mqtt_probe_credentials_configuration",
            "mqtt_probe_unavailable", "mqtt_probe_allocation",
            "mqtt_library_unavailable"
        };
        // Parsing/checking the worker's response is also within the
        // end-to-end deadline. Never return late success or mislabel it as
        // an internal worker error.
        if (leftMs(deadline) == 0) return rejected("mqtt_probe_timeout");
        if (!codes.count(result.code) || result.code.size() > 64 ||
            result.success != result.code.empty() ||
            (result.success &&
             (result.tlsVerified != connection.mqtt.tls ||
              result.credentialsAccepted != !login.user.empty())))
            return rejected("mqtt_probe_worker_unavailable");
        return result;
    } catch (const std::exception&) {
        return rejected("mqtt_probe_worker_unavailable");
    }
}
struct ProbeEvent {
    std::mutex mutex;
    std::condition_variable cv;
    bool finished = false;
    bool disconnected = false;
    int connack = -1;
};
void connected(mosquitto*, void* context, int code) noexcept {
    try {
        auto& e = *static_cast<ProbeEvent*>(context);
        {
            std::lock_guard<std::mutex> lock(e.mutex);
            if (!e.finished) { e.connack = code; e.finished = true; }
        }
        e.cv.notify_one();
    } catch (...) {}
}
void disconnected(mosquitto*, void* context, int) noexcept {
    try {
        auto& e = *static_cast<ProbeEvent*>(context);
        {
            std::lock_guard<std::mutex> lock(e.mutex);
            e.disconnected = true;
            e.finished = true;
        }
        e.cv.notify_one();
    } catch (...) {}
}
struct ProbeClient {
    mosquitto* ptr = nullptr;
    bool running = false;
    explicit ProbeClient(ProbeEvent* event) {
        detail::ensureMqttLibrary();
        ptr = mosquitto_new(nullptr, true, event);
        if (!ptr) throw std::runtime_error("mqtt_probe_allocation");
    }
    ~ProbeClient() {
        if (!ptr) return;
        if (running) {
            (void)mosquitto_disconnect(ptr);
            (void)mosquitto_loop_stop(ptr, true);
        }
        mosquitto_destroy(ptr);
    }
};
#endif
} // namespace

MqttProbeResult MqttConnectionProbe::testConnection(
    const config::Connection& connection, const MqttCredentials& credentials) {
    if (!validConnection(connection)) return rejected("mqtt_probe_invalid_connection");
#ifndef FIREFLY_WITH_MOSQUITTO
    (void)credentials;
    return rejected("mqtt_unavailable");
#else
    ProbeSlot slot;
    if (!slot.acquired) return rejected("mqtt_probe_busy");
    const auto deadline = Clock::now() + std::chrono::milliseconds(connection.timeoutMs);
    try {
        const auto login = credentials.read(connection.mqtt.credentialsId);
        // The supervised process can be SIGKILLed even while libc resolves
        // the original hostname. It uses unchanged host for TLS/SNI checking.
        return subprocessProbe(connection, login, deadline);
    } catch (const std::exception& error) {
        const std::string code(error.what());
        if (code.rfind("mqtt_credentials_", 0) == 0) return rejected(code);
        return rejected("mqtt_probe_worker_unavailable");
    }
#endif
}

MqttProbeResult detail::runMqttProbeWorker(const config::Connection& connection,
                                           const MqttLogin& login) {
    if (!validConnection(connection)) return rejected("mqtt_probe_invalid_connection");
#ifndef FIREFLY_WITH_MOSQUITTO
    (void)login;
    return rejected("mqtt_unavailable");
#else
    try {
        ProbeEvent event;
        ProbeClient client(&event);
        if (connection.mqtt.tls) {
            if (mosquitto_tls_set(client.ptr, nullptr, "/etc/ssl/certs",
                                  nullptr, nullptr, nullptr) != MOSQ_ERR_SUCCESS ||
                mosquitto_tls_insecure_set(client.ptr, false) != MOSQ_ERR_SUCCESS)
                return rejected("mqtt_probe_tls_configuration");
        }
        if (!login.user.empty() &&
            mosquitto_username_pw_set(client.ptr, login.user.c_str(),
                                      login.password.c_str()) != MOSQ_ERR_SUCCESS)
            return rejected("mqtt_probe_credentials_configuration");
        mosquitto_connect_callback_set(client.ptr, connected);
        mosquitto_disconnect_callback_set(client.ptr, disconnected);
        // No MQTT publish or subscribe path exists in this worker.
        if (mosquitto_connect_async(client.ptr, connection.network.host.c_str(),
                                    static_cast<int>(connection.network.port), 30) != MOSQ_ERR_SUCCESS ||
            mosquitto_loop_start(client.ptr) != MOSQ_ERR_SUCCESS)
            return rejected("mqtt_probe_connection_failed");
        client.running = true;
        const auto deadline = Clock::now() + std::chrono::milliseconds(connection.timeoutMs);
        std::unique_lock<std::mutex> lock(event.mutex);
        if (!event.cv.wait_until(lock, deadline, [&] { return event.finished; }))
            return rejected("mqtt_probe_timeout");
        const auto connack = event.connack;
        const auto lost = event.disconnected;
        lock.unlock();
        if (connack == 4 || connack == 5) return rejected("mqtt_probe_auth_rejected");
        if (connack != 0) return rejected(connack == -1 ? "mqtt_probe_connection_failed" :
                                         "mqtt_probe_connack_rejected");
        if (lost) return rejected("mqtt_probe_connection_failed");
        return {true, {}, connection.mqtt.tls, !login.user.empty()};
    } catch (const std::exception& error) {
        const std::string code(error.what());
        if (code == "mqtt_library_unavailable" || code == "mqtt_probe_allocation")
            return rejected(code);
        return rejected("mqtt_probe_unavailable");
    }
#endif
}
} // namespace firefly::execution
