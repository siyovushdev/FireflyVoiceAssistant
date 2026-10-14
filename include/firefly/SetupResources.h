#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <string>

namespace firefly {
// Shared by the model owner and Admin STT. No sampling thread or device access.
class SetupResources {
public:
    static constexpr std::uint64_t GiBKiB = 1024 * 1024;
    struct Memory {
        std::uint64_t availableKiB = 0, pssKiB = 0, dmaBytes = 0;
        std::uint64_t cgroupCurrentBytes = 0;
        std::uint64_t cgroupHeadroomKiB = std::numeric_limits<std::uint64_t>::max();
    };
    struct Hooks {
        std::function<Memory()> read;
        std::function<std::chrono::steady_clock::time_point()> now;
        std::function<void(std::chrono::milliseconds)> wait;
        std::function<void(const std::string&)> log;
    };
    explicit SetupResources(Hooks hooks = {});
    std::unique_lock<std::timed_mutex> acquire(std::chrono::milliseconds timeout = std::chrono::seconds(45));
    Memory checkpoint(const std::string& phase, std::uint64_t minimumKiB = 0);
    void note(const std::string& phase) noexcept;
    void awaitReleased(std::uint64_t baselineDmaBytes);
    void awaitAdmission(std::uint64_t minimumKiB);
    static Memory readLinux(const std::string& proc = "/proc", const std::string& cgroup = "/sys/fs/cgroup");
private:
    Hooks hooks_;
    std::timed_mutex mutex_;
};
}
