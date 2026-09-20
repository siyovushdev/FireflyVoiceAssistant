#include "Ipc.h"
#include "Json.h"
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <thread>

extern char** environ;
namespace firefly::admin {
namespace {
void transfer(int fd, char* bytes, std::size_t size, bool writing, int timeoutSeconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    while (size) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) throw std::runtime_error("Runtime IPC deadline");
        pollfd descriptor{fd, static_cast<short>(writing ? POLLOUT : POLLIN), 0};
        const int result = ::poll(&descriptor, 1, static_cast<int>(remaining));
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) throw std::runtime_error("Runtime IPC unavailable or timed out");
        const auto count = writing ? ::send(fd, bytes, size, MSG_NOSIGNAL | MSG_DONTWAIT) : ::recv(fd, bytes, size, MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count <= 0) throw std::runtime_error("Runtime IPC closed");
        bytes += count; size -= static_cast<std::size_t>(count);
    }
}
}
std::string receiveMessage(int fd, int timeoutSeconds) {
    std::uint32_t size;
    transfer(fd, reinterpret_cast<char*>(&size), sizeof(size), false, timeoutSeconds);
    size = ntohl(size);
    if (size == 0 || size > MaxMessageBytes) throw std::runtime_error("Invalid runtime IPC size");
    std::string message(size, '\0');
    transfer(fd, message.data(), message.size(), false, timeoutSeconds);
    return message;
}
void sendMessage(int fd, const std::string& message, int timeoutSeconds) {
    if (message.empty() || message.size() > MaxMessageBytes) throw std::runtime_error("Invalid runtime IPC size");
    auto size = htonl(static_cast<std::uint32_t>(message.size()));
    transfer(fd, reinterpret_cast<char*>(&size), sizeof(size), true, timeoutSeconds);
    transfer(fd, const_cast<char*>(message.data()), message.size(), true, timeoutSeconds);
}
ChildRuntime::ChildRuntime(const std::string& executable, const std::string& model) {
    int pair[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair)) throw std::runtime_error("Cannot create runtime socketpair");
    posix_spawn_file_actions_t actions;
    ::posix_spawn_file_actions_init(&actions);
    // Close the parent endpoint first: it can be fd 3, the child's fixed endpoint.
    ::posix_spawn_file_actions_addclose(&actions, pair[0]);
    ::posix_spawn_file_actions_adddup2(&actions, pair[1], 3);
    if (pair[1] != 3) ::posix_spawn_file_actions_addclose(&actions, pair[1]);
    char* arguments[]{const_cast<char*>(executable.c_str()), const_cast<char*>("--model"), const_cast<char*>(model.c_str()), nullptr};
    const int result = ::posix_spawn(&pid_, executable.c_str(), &actions, nullptr, arguments, environ);
    ::posix_spawn_file_actions_destroy(&actions);
    ::close(pair[1]);
    if (result) { ::close(pair[0]); pid_ = -1; throw std::runtime_error("Cannot start inference worker: " + std::string(std::strerror(result))); }
    fd_ = pair[0];
}
ChildRuntime::~ChildRuntime() {
    if (fd_ >= 0) ::close(fd_);
    if (pid_ > 0) {
        // Owned child only. Give normal socket EOF/destruction a short grace period.
        for (int i = 0; i < 100; ++i) {
            if (::waitpid(pid_, nullptr, WNOHANG) == pid_) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        ::kill(pid_, SIGTERM);
        for (int i = 0; i < 20; ++i) {
            if (::waitpid(pid_, nullptr, WNOHANG) == pid_) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        ::kill(pid_, SIGKILL);
        while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
    }
}
std::string ChildRuntime::call(std::string_view request) {
    if (fd_ < 0) throw std::runtime_error("Runtime IPC requires service restart");
    try {
        sendMessage(fd_, std::string(request), 5);
        return receiveMessage(fd_, 120);
    } catch (...) {
        // A late reply must never be mistaken for the next operation's reply.
        ::close(fd_); fd_ = -1;
        if (pid_ > 0) ::kill(pid_, SIGTERM);
        throw;
    }
}
}
