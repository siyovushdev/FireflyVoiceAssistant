#pragma once
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <fcntl.h>
#include <limits.h>
#include <nlohmann/json.hpp>
#include <new>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace firefly::execution::detail {
using MqttClock = std::chrono::steady_clock;
struct MqttFd {
    int value = -1;
    explicit MqttFd(int fd = -1) : value(fd) {}
    ~MqttFd() { if (value >= 0) (void)::close(value); }
    MqttFd(const MqttFd&) = delete;
    MqttFd& operator=(const MqttFd&) = delete;
};
// A Linux process-shared lock-free arrival counter. No credentials or state are
// stored here. The worker takes its barrier immediately before mosquitto_publish;
// parent callbacks use this SAME counter, even while the IPC gate mutex is held.
class MqttSharedOrder {
    MqttFd fd_;
    std::atomic<std::uint64_t>* counter_ = nullptr;
public:
    MqttSharedOrder() : fd_(::memfd_create("firefly-mqtt-order", MFD_CLOEXEC | MFD_ALLOW_SEALING)) {
        static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                      "MQTT process ordering requires a lock-free 64-bit counter");
        if (fd_.value < 0 || ::fchmod(fd_.value, 0600) ||
            ::ftruncate(fd_.value, sizeof(*counter_))) return;
        auto* memory = ::mmap(nullptr, sizeof(*counter_), PROT_READ | PROT_WRITE, MAP_SHARED, fd_.value, 0);
        if (memory == MAP_FAILED) return;
        counter_ = new (memory) std::atomic<std::uint64_t>(1);
        if (::fcntl(fd_.value, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0) {
            ::munmap(counter_, sizeof(*counter_)); counter_ = nullptr;
        }
    }
    explicit MqttSharedOrder(int inherited) : fd_(::fcntl(inherited, F_DUPFD_CLOEXEC, 4)) {
        struct stat st{};
        const int seals = fd_.value < 0 ? -1 : ::fcntl(fd_.value, F_GET_SEALS);
        if (fd_.value < 0 || ::fstat(fd_.value, &st) || !S_ISREG(st.st_mode) ||
            st.st_uid != ::geteuid() || (st.st_mode & 0777) != 0600 ||
            st.st_size != sizeof(*counter_) || seals < 0 ||
            (seals & (F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL)) !=
                     (F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL)) return;
        auto* memory = ::mmap(nullptr, sizeof(*counter_), PROT_READ | PROT_WRITE, MAP_SHARED, fd_.value, 0);
        if (memory != MAP_FAILED) counter_ = static_cast<std::atomic<std::uint64_t>*>(memory);
    }
    ~MqttSharedOrder() { if (counter_) ::munmap(counter_, sizeof(*counter_)); }
    bool valid() const { return counter_ != nullptr; }
    int descriptor() const { return valid() ? fd_.value : -1; }
    std::uint64_t next() const { return counter_->fetch_add(1, std::memory_order_seq_cst); }
    std::uint64_t current() const { return counter_->load(std::memory_order_seq_cst); }
};
inline int mqttRemaining(MqttClock::time_point deadline) {
    const auto now = MqttClock::now();
    if (now >= deadline) return 0;
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
    return static_cast<int>(std::min<std::int64_t>(ms, INT_MAX));
}
inline bool mqttReady(int fd, short events, MqttClock::time_point deadline) {
    while (const int ms = mqttRemaining(deadline)) {
        pollfd p{fd, events, 0};
        const int n = ::poll(&p, 1, ms);
        if ((n < 0 && errno == EINTR) || n == 0) continue;
        if (n < 0 || (p.revents & (POLLERR | POLLNVAL))) return false;
        if (p.revents & (events | POLLHUP)) return true;
    }
    return false;
}
inline bool mqttBytes(int fd, void* bytes, std::size_t size, bool writing, MqttClock::time_point deadline) {
    auto* data = static_cast<char*>(bytes);
    for (std::size_t pos = 0; pos < size;) {
        if (!mqttReady(fd, writing ? POLLOUT : POLLIN, deadline)) return false;
        const auto n = writing ? ::send(fd, data + pos, size - pos, MSG_NOSIGNAL | MSG_DONTWAIT)
                               : ::recv(fd, data + pos, size - pos, MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) return false;
        pos += static_cast<std::size_t>(n);
    }
    return true;
}
inline bool mqttSend(int fd, const nlohmann::json& value, MqttClock::time_point deadline) {
    auto body = value.dump();
    if (body.size() > 32768) return false;
    std::uint32_t size = static_cast<std::uint32_t>(body.size());
    unsigned char header[]{static_cast<unsigned char>(size >> 24), static_cast<unsigned char>(size >> 16),
                           static_cast<unsigned char>(size >> 8), static_cast<unsigned char>(size)};
    return mqttBytes(fd, header, 4, true, deadline) && mqttBytes(fd, body.data(), body.size(), true, deadline);
}
inline bool mqttReceive(int fd, nlohmann::json& value, MqttClock::time_point deadline, unsigned limit = 1024) {
    unsigned char header[4]{};
    if (!mqttBytes(fd, header, 4, false, deadline)) return false;
    const auto size = (unsigned(header[0]) << 24) | (unsigned(header[1]) << 16) |
                      (unsigned(header[2]) << 8) | header[3];
    if (!size || size > limit) return false;
    std::string body(size, '\0');
    if (!mqttBytes(fd, body.data(), size, false, deadline)) return false;
    value = nlohmann::json::parse(body, nullptr, false);
    return value.is_object();
}
struct MqttChild {
    pid_t pid = -1;
    bool reaped = false;
    explicit MqttChild(pid_t value) : pid(value) {}
    MqttChild(const MqttChild&) = delete;
    MqttChild& operator=(const MqttChild&) = delete;
    ~MqttChild() {
        if (pid <= 0 || reaped) return;
        (void)::kill(pid, SIGKILL);
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    bool reap(MqttClock::time_point deadline) {
        while (mqttRemaining(deadline)) {
            int status = 0;
            const auto result = ::waitpid(pid, &status, WNOHANG);
            if (result == pid) { reaped = true; return WIFEXITED(status) && WEXITSTATUS(status) == 0; }
            if (result < 0 && errno != EINTR) return false;
            (void)::poll(nullptr, 0, std::min(2, mqttRemaining(deadline)));
        }
        return false;
    }
};
inline std::string mqttPublishWorkerPath() {
    char path[PATH_MAX + 1]{};
    const auto size = ::readlink("/proc/self/exe", path, PATH_MAX);
    if (size <= 0 || size >= PATH_MAX) throw std::runtime_error("mqtt_publish_worker_unavailable");
    std::string worker(path, static_cast<std::size_t>(size));
    worker = worker.substr(0, worker.find_last_of('/') + 1) + "firefly-mqtt-publish-worker";
    struct stat st{};
    if (::lstat(worker.c_str(), &st) || !S_ISREG(st.st_mode) ||
        (st.st_uid != ::geteuid() && st.st_uid != 0) ||
        (st.st_mode & (S_ISUID | S_ISGID | 022)) || !(st.st_mode & 0111))
        throw std::runtime_error("mqtt_publish_worker_unavailable");
    return worker;
}
} // namespace firefly::execution::detail
