#include "firefly/voice/AminaWakeDetector.h"
#include "firefly/voice/Speech.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
namespace firefly::voice {
struct AminaWakeDetector::Impl {
    int socket = -1;
    pid_t child = -1;
    ~Impl() {
        if (socket >= 0) { ::shutdown(socket, SHUT_RDWR); ::close(socket); }
        if (child > 0) {
            // EOF normally exits the worker. Bound the grace period for native hangs.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            while (std::chrono::steady_clock::now() < deadline) {
                const auto result = ::waitpid(child, nullptr, WNOHANG);
                if (result == child || (result < 0 && errno == ECHILD)) return;
                ::poll(nullptr, 0, 10);
            }
            ::kill(child, SIGKILL);
            while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
        }
    }
    char reply(int timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) throw std::runtime_error("Wake worker deadline exceeded");
            pollfd descriptor{socket, POLLIN, 0};
            const int rc = ::poll(&descriptor, 1, static_cast<int>(remaining));
            if (rc < 0 && errno == EINTR) continue;
            if (rc <= 0) throw std::runtime_error("Wake worker unavailable or timed out");
            char byte;
            if (::recv(socket, &byte, 1, MSG_DONTWAIT) != 1)
                throw std::runtime_error("Wake worker disconnected");
            return byte;
        }
    }
    void send(const void* data, size_t size) {
        // Requests fit into a fresh socket buffer; never wait on a dead/stalled worker.
        if (::send(socket, data, size, MSG_NOSIGNAL | MSG_DONTWAIT) != static_cast<ssize_t>(size))
            throw std::runtime_error("Wake worker write failed");
    }
};
AminaWakeDetector::AminaWakeDetector(const WakeConfig& config, const std::string& python,
                                   const std::string& worker) : impl_(std::make_unique<Impl>()) {
    config.validate();
    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0)
        throw std::runtime_error("Cannot create wake IPC");
    impl_->socket = sockets[0];
    posix_spawn_file_actions_t actions;
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc != 0) { ::close(sockets[1]); throw std::runtime_error("Cannot initialize wake spawn"); }
    rc = posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO);
    if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, sockets[1], STDOUT_FILENO);
    // Ubuntu 22.04/glibc: never inherit RKLLM, HTTP, or other product descriptors.
    if (!rc) rc = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
    const std::string cutoff = std::to_string(config.cutoff), window = std::to_string(config.window);
    std::array<char*, 9> argv{const_cast<char*>(python.c_str()), const_cast<char*>(worker.c_str()),
        const_cast<char*>("--model"), const_cast<char*>(config.modelPath.c_str()),
        const_cast<char*>("--cutoff"), const_cast<char*>(cutoff.c_str()),
        const_cast<char*>("--window"), const_cast<char*>(window.c_str()), nullptr};
    if (!rc) rc = ::posix_spawnp(&impl_->child, python.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(sockets[1]);
    if (rc != 0) throw std::runtime_error("Cannot spawn wake worker");
    if (impl_->reply(10000) != 'R') throw std::runtime_error("Invalid wake worker startup");
}
AminaWakeDetector::~AminaWakeDetector() = default;
void AminaWakeDetector::reset() {
    const char request = 'R'; impl_->send(&request, 1);
    if (impl_->reply(1000) != 'R') throw std::runtime_error("Invalid wake reset response");
}
bool AminaWakeDetector::accept(const float* samples, int count) {
    if (count != FrameSamples) throw std::invalid_argument("Wake requires 512 PCM samples");
    std::array<unsigned char, 1 + FrameSamples * 2> request{};
    request[0] = 'A';
    for (int i = 0; i < count; ++i) {
        if (!std::isfinite(samples[i])) throw std::invalid_argument("Non-finite wake PCM");
        const auto pcm = static_cast<int16_t>(std::clamp(samples[i] * 32768.0f, -32768.0f, 32767.0f));
        const auto bits = static_cast<uint16_t>(pcm);
        request[1 + 2 * i] = bits & 255; request[2 + 2 * i] = bits >> 8;
    }
    impl_->send(request.data(), request.size());
    const char reply = impl_->reply(500);
    if (reply != 'W' && reply != 'N') throw std::runtime_error("Invalid wake inference response");
    return reply == 'W';
}
}
