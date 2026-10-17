// Socket-activated privileged adapter for one pre-provisioned systemd unit.
// DO NOT mark setuid. Installs only through a separate owner-approved image
// provisioning step; Firefly Web Admin itself never gets root privileges.
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <pwd.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
constexpr const char* Unit = "firefly-mqtt-broker.service";
constexpr int RequestTimeoutMs = 2000;
constexpr int CommandTimeoutMs = 5000;
volatile std::sig_atomic_t stopped = 0;
void terminate(int) { stopped = 1; }
struct Fd {
    int fd = -1;
    explicit Fd(int v = -1) : fd(v) {}
    ~Fd() { if (fd >= 0) (void)::close(fd); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
struct CommandResult { bool success = false; std::string output; };
bool waitFor(int fd, short events, int timeout) {
    pollfd p{fd, events, 0};
    for (;;) {
        const int result = ::poll(&p, 1, timeout);
        if (result < 0 && errno == EINTR) continue;
        return result > 0 && (p.revents & events) && !(p.revents & POLLNVAL);
    }
}
CommandResult runSystemctl(const std::vector<std::string>& arguments) {
    // Compile-time fixed executable in production. Only the test binary
    // permits selecting a synthetic systemctl via an environment variable.
    const char* binary = "/usr/bin/systemctl";
#ifdef FIREFLY_MQTT_CONTROL_TEST_ONLY
    binary = ::getenv("FIREFLY_TEST_SYSTEMCTL");
    if (!binary || binary[0] != '/') return {};
#endif
    int pipes[2];
    if (::pipe2(pipes, O_CLOEXEC | O_NONBLOCK)) return {};
    Fd readEnd(pipes[0]);
    Fd writeEnd(pipes[1]);
    const pid_t pid = ::fork();
    if (pid < 0) return {};
    if (pid == 0) {
        (void)::dup2(writeEnd.fd, STDOUT_FILENO);
        (void)::dup2(writeEnd.fd, STDERR_FILENO);
        (void)::close(readEnd.fd);
        (void)::close(writeEnd.fd);
        // No untrusted argv, no shell, no inherited attacker environment.
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(binary));
        for (const auto& a : arguments) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        char path[] = "PATH=/usr/bin:/bin";
        char locale[] = "LC_ALL=C";
        char* envp[]{path, locale, nullptr};
        ::execve(binary, argv.data(), envp);
        _exit(127);
    }
    (void)::close(writeEnd.fd); writeEnd.fd = -1; // close parent copy
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(CommandTimeoutMs);
    std::string output;
    int status = 0;
    bool finished = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!finished) {
            const pid_t check = ::waitpid(pid, &status, WNOHANG);
            if (check == pid) finished = true;
            if (check < 0) break;
        }
        if (waitFor(readEnd.fd, POLLIN, 50)) {
            std::array<char, 128> buffer{};
            const ssize_t n = ::read(readEnd.fd, buffer.data(), buffer.size());
            if (n > 0) {
                if (output.size() + static_cast<std::size_t>(n) > 512) break;
                output.append(buffer.data(), static_cast<std::size_t>(n));
            }
        }
        if (finished) {
            for (;;) {
                std::array<char, 128> buffer{};
                const auto n = ::read(readEnd.fd, buffer.data(), buffer.size());
                if (n <= 0) break;
                if (output.size() + static_cast<std::size_t>(n) > 512) break;
                output.append(buffer.data(), static_cast<std::size_t>(n));
            }
            break;
        }
    }
    if (!finished) {
        (void)::kill(pid, SIGKILL);
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        return {};
    }
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
        output.pop_back();
    return {WIFEXITED(status) && WEXITSTATUS(status) == 0, output};
}
struct Status {
    bool installed = false;
    bool enabled = false;
    bool running = false;
    bool valid = false;
};
Status inspect() {
    const auto enablement = runSystemctl({"--system", "is-enabled", Unit});
    if (enablement.output == "not-found") return {false, false, false, true};
    if (enablement.output != "enabled" && enablement.output != "disabled")
        return {};
    const auto activity = runSystemctl({"--system", "is-active", Unit});
    if (activity.output != "active" && activity.output != "inactive" &&
        activity.output != "failed") return {};
    return {true, enablement.output == "enabled", activity.output == "active", true};
}
std::string stateLine(const Status& s) {
    if (!s.valid) return "ERROR broker_invalid_state\n";
    if (!s.installed) return "STATE missing disabled stopped\n";
    return std::string("STATE present ") +
           (s.enabled ? "enabled " : "disabled ") +
           (s.running ? "running\n" : "stopped\n");
}
std::string executeRequest(const std::string& request) {
    if (request != "STATUS\n" && request != "ENABLE\n" && request != "DISABLE\n")
        return "ERROR broker_invalid_state\n";
    if (request == "STATUS\n") return stateLine(inspect());
    const auto before = inspect();
    if (!before.valid) return "ERROR broker_invalid_state\n";
    if (!before.installed) return "ERROR broker_not_installed\n";
    const auto result = request == "ENABLE\n"
        ? runSystemctl({"--system", "enable", "--now", Unit})
        : runSystemctl({"--system", "disable", "--now", Unit});
    if (!result.success) return "ERROR broker_control_failed\n";
    const auto after = inspect();
    if (!after.valid) return "ERROR broker_invalid_state\n";
    if (!after.installed) return "ERROR broker_control_failed\n";
    if (request == "ENABLE\n" && (!after.enabled || !after.running))
        return "ERROR broker_control_failed\n";
    if (request == "DISABLE\n" && (after.enabled || after.running))
        return "ERROR broker_control_failed\n";
    return stateLine(after);
}
unsigned allowedUid() {
#ifdef FIREFLY_MQTT_CONTROL_TEST_ONLY
    return static_cast<unsigned>(::geteuid());
#else
    if (::geteuid() != 0) throw std::runtime_error("not_root");
    const passwd* user = ::getpwnam("firefly");
    if (!user) throw std::runtime_error("firefly_user_missing");
    return static_cast<unsigned>(user->pw_uid);
#endif
}
void serve(int client, unsigned uid) {
    struct ucred peer{};
    socklen_t size = sizeof(peer);
    if (::getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &size) ||
        size != sizeof(peer) || peer.uid != uid) return;
    // AF_UNIX/SOCK_STREAM is not record-framed. Accumulate a very short
    // complete request with one absolute deadline; never accept a partial
    // STATUS/ENABLE/DISABLE because the kernel split a single send().
    std::string message;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(RequestTimeoutMs);
    while (message.find('\n') == std::string::npos && message.size() < 32) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0 || !waitFor(client, POLLIN, static_cast<int>(left))) return;
        std::array<char, 32> request{};
        const auto got = ::recv(client, request.data(),
                                std::min(request.size(), 32-message.size()), MSG_DONTWAIT);
        if (got < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (got <= 0) return;
        message.append(request.data(), static_cast<std::size_t>(got));
    }
    // Reject injected extra lines, NULs, suffixes and unknown operations.
    const auto response = executeRequest(message);
    if (waitFor(client, POLLOUT, RequestTimeoutMs)) {
        std::size_t offset = 0;
        while (offset < response.size()) {
            const auto n = ::send(client, response.data() + offset,
                                  response.size()-offset, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (n <= 0) break;
            offset += static_cast<std::size_t>(n);
        }
    }
}
} // namespace

int main() {
    try {
        // systemd ListenStream socket activation: fd 3 is our only listener.
        // A request cannot choose a listening path, executable or unit name.
        constexpr int listenFd = 3;
        struct stat st{};
        if (::fstat(listenFd, &st) || !S_ISSOCK(st.st_mode))
            return 2;
        const unsigned uid = allowedUid();
        ::signal(SIGTERM, terminate);
        ::signal(SIGINT, terminate);
        while (!stopped) {
            if (!waitFor(listenFd, POLLIN, 1000)) continue;
            Fd client(::accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK));
            if (client.fd < 0) continue;
            serve(client.fd, uid);
        }
        return 0;
    } catch (const std::exception&) {
        return 3;
    }
}
