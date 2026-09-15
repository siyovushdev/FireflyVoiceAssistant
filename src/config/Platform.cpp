#include "Platform.h"
#include "firefly/config/Configuration.h"
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <linux/if_alg.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace firefly::config::detail {
FileDescriptor::~FileDescriptor() { if (value_ >= 0) ::close(value_); }
FileDescriptor::FileDescriptor(FileDescriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
FileDescriptor& FileDescriptor::operator=(FileDescriptor&& other) noexcept {
    if (this != &other) {
        if (value_ >= 0) ::close(value_);
        value_ = std::exchange(other.value_, -1);
    }
    return *this;
}
[[noreturn]] void ioError(const std::string& operation) {
    throw ConfigError("io_error", "", operation + ": " + std::strerror(errno));
}
FileDescriptor openDirectory(const std::string& path) {
    if (path.empty()) throw ConfigError("invalid_path", "", "Explicit directory required");
    const std::filesystem::path requested(path);
    FileDescriptor directory(::open(requested.is_absolute() ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (directory.get() < 0) ioError("open directory root");
    for (const auto& component : requested) {
        const auto name = component.string();
        if (name == "/" || name == "." || name.empty()) continue;
        if (name == ".." || name.find('\0') != std::string::npos)
            throw ConfigError("invalid_path", "", "Parent traversal and NUL paths forbidden");
        FileDescriptor next(::openat(directory.get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (next.get() < 0) ioError("open directory component " + name);
        directory = std::move(next);
    }
    return directory;
}
std::string readAt(int directory, const std::string& name) {
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos || name.find('\0') != std::string::npos)
        throw ConfigError("invalid_path", "", "Expected one file name");
    FileDescriptor file(::openat(directory, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (file.get() < 0) ioError("open " + name);
    return readDescriptor(file.get());
}
std::string readDescriptor(int file) {
    struct stat status{};
    if (::fstat(file, &status) != 0) ioError("stat configuration file");
    if (!S_ISREG(status.st_mode)) throw ConfigError("invalid_file", "", "Expected a regular file");
    if (status.st_size < 0 || status.st_size > static_cast<off_t>(MaxConfigBytes))
        throw ConfigError("size_limit", "", "Configuration exceeds 256 KiB");
    std::string result;
    std::array<char, 4096> buffer{};
    while (true) {
        const auto count = ::read(file, buffer.data(), buffer.size());
        if (count < 0) { if (errno == EINTR) continue; ioError("read configuration file"); }
        if (count == 0) break;
        if (result.size() + static_cast<std::size_t>(count) > MaxConfigBytes)
            throw ConfigError("size_limit", "", "Configuration exceeds 256 KiB");
        result.append(buffer.data(), static_cast<std::size_t>(count));
    }
    return result;
}
std::string readFile(const std::string& path) {
    const std::filesystem::path requested(path);
    auto directory = openDirectory(requested.has_parent_path() ? requested.parent_path().string() : ".");
    return readAt(directory.get(), requested.filename().string());
}
std::string sha256(std::string_view bytes) {
    FileDescriptor algorithm(::socket(AF_ALG, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
    if (algorithm.get() < 0) ioError("SHA-256 socket");
    sockaddr_alg address{};
    address.salg_family = AF_ALG;
    std::strcpy(reinterpret_cast<char*>(address.salg_type), "hash");
    std::strcpy(reinterpret_cast<char*>(address.salg_name), "sha256");
    if (::bind(algorithm.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
        ioError("SHA-256 bind");
    FileDescriptor operation(::accept4(algorithm.get(), nullptr, nullptr, SOCK_CLOEXEC));
    if (operation.get() < 0) ioError("SHA-256 accept");
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::send(operation.get(), bytes.data() + offset, bytes.size() - offset, MSG_MORE | MSG_NOSIGNAL);
        if (count < 0) { if (errno == EINTR) continue; ioError("SHA-256 send"); }
        if (count == 0) throw ConfigError("io_error", "", "SHA-256 short send");
        offset += static_cast<std::size_t>(count);
    }
    ssize_t count;
    do { count = ::send(operation.get(), nullptr, 0, MSG_NOSIGNAL); } while (count < 0 && errno == EINTR);
    if (count < 0) ioError("SHA-256 finalize");
    std::array<unsigned char, 32> digest{};
    do { count = ::read(operation.get(), digest.data(), digest.size()); } while (count < 0 && errno == EINTR);
    if (count != static_cast<ssize_t>(digest.size())) throw ConfigError("io_error", "", "SHA-256 digest unavailable");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
void checkText(std::string_view text, const std::string& path) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        std::uint32_t code = first;
        int remaining = 0;
        if (first >= 0xc2 && first <= 0xdf) { code = first & 0x1f; remaining = 1; }
        else if (first >= 0xe0 && first <= 0xef) { code = first & 0x0f; remaining = 2; }
        else if (first >= 0xf0 && first <= 0xf4) { code = first & 0x07; remaining = 3; }
        else if (first >= 0x80) throw ConfigError("invalid_text", path, "Invalid UTF-8");
        const auto minimum = remaining == 1 ? 0x80u : remaining == 2 ? 0x800u : remaining == 3 ? 0x10000u : 0u;
        while (remaining-- > 0) {
            if (i == text.size()) throw ConfigError("invalid_text", path, "Truncated UTF-8");
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) throw ConfigError("invalid_text", path, "Invalid UTF-8 continuation");
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
            throw ConfigError("invalid_text", path, "Invalid Unicode scalar");
        if (code < 0x20 || (code >= 0x7f && code <= 0x9f) || code == 0x2028 || code == 0x2029 ||
            (code >= 0x202a && code <= 0x202e) || (code >= 0x2066 && code <= 0x2069) || code == 0xfeff)
            throw ConfigError("invalid_text", path, "Control/format character forbidden");
    }
    if (text.find("<|") != std::string_view::npos || text.find("|>") != std::string_view::npos)
        throw ConfigError("invalid_text", path, "Chat control sequence forbidden");
}
std::string lowercase(std::string text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto first = static_cast<unsigned char>(text[i]);
        if (first >= 'A' && first <= 'Z') text[i] += 'a' - 'A';
        if (first != 0xd0 || i + 1 == text.size()) continue;
        const auto second = static_cast<unsigned char>(text[i + 1]);
        if (second >= 0x90 && second <= 0x9f) text[i + 1] = static_cast<char>(second + 0x20);
        else if (second >= 0xa0 && second <= 0xaf) { text[i] = static_cast<char>(0xd1); text[i + 1] = static_cast<char>(second - 0x20); }
        else if (second == 0x81) { text[i] = static_cast<char>(0xd1); text[i + 1] = static_cast<char>(0x91); }
    }
    return text;
}
}
