#include "firefly/voice/WakeDirectionClient.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace firefly::voice {
WakeDirectionClient::WakeDirectionClient(const std::string& path) : path_(path) {
    if (path.empty()) return;
    if (path.front() != '/' || path.size() >= sizeof(address_.sun_path) ||
        path.find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid wake direction socket path");
    const auto parent = std::filesystem::path(path).parent_path().string();
    struct stat metadata{};
    if (::lstat(parent.c_str(), &metadata) != 0 || !S_ISDIR(metadata.st_mode) ||
        (metadata.st_uid != ::geteuid() && metadata.st_uid != 0) || (metadata.st_mode & 0022))
        throw std::invalid_argument("Wake direction socket requires a private owned directory");
    address_.sun_family = AF_UNIX;
    std::memcpy(address_.sun_path, path.c_str(), path.size() + 1);
    descriptor_ = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (descriptor_ < 0) throw std::runtime_error("Cannot open wake direction channel");
}
WakeDirectionClient::~WakeDirectionClient() { if (descriptor_ >= 0) ::close(descriptor_); }

bool WakeDirectionClient::notify(std::string_view state) const noexcept {
    if (descriptor_ < 0) return false;
    bool allowed = false;
    for (const auto* name : {"WAKE_DETECTED", "WAKE_REPEATED", "WAITING_WAKE", "PROCESSING", "COMMAND_START_TIMEOUT",
                            "COMMAND_TIMEOUT", "COMMAND_MAX_DURATION", "WAKE_UNAVAILABLE", "stopping", "stopped"})
        if (state == name) { allowed = true; break; }
    if (!allowed) return false;
    struct stat metadata{};
    if (::lstat(path_.c_str(), &metadata) != 0 || !S_ISSOCK(metadata.st_mode) ||
        (metadata.st_uid != ::geteuid() && metadata.st_uid != 0)) return false;
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    char packet[96];
    const auto size = std::snprintf(packet, sizeof(packet), "%.*s %lld",
        static_cast<int>(state.size()), state.data(), static_cast<long long>(milliseconds));
    if (size <= 0 || static_cast<std::size_t>(size) >= sizeof(packet)) return false;
    return ::sendto(descriptor_, packet, static_cast<std::size_t>(size), MSG_DONTWAIT | MSG_NOSIGNAL,
        reinterpret_cast<const sockaddr*>(&address_), sizeof(address_)) == size;
}
}
