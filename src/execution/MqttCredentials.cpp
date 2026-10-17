#include "firefly/execution/MqttCredentials.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <utility>
#include <unistd.h>

namespace firefly::execution {
namespace {
[[noreturn]] void reject(const char* code) { throw std::runtime_error(code); }

bool validId(const std::string& id) {
    if (id.empty() || id.size() > 64 || id.front() < 'a' || id.front() > 'z') return false;
    return id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos;
}
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
            value = other.value;
            other.value = -1;
        }
        return *this;
    }
};
Fd openDirectory(const std::string& path) {
    // Traverse every path component using no-follow directory FDs. A symlink
    // anywhere in the operator-provisioned hierarchy fails closed, rather than
    // letting a renamed parent redirect secret operations.
    if (path.empty() || path.front() != '/' || path.back() == '/')
        reject("mqtt_credentials_directory_invalid");
    Fd current(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (current.value < 0) reject("mqtt_credentials_directory_unavailable");
    std::size_t pos = 1;
    while (pos < path.size()) {
        const auto slash = path.find('/', pos);
        const auto part = path.substr(pos, slash == std::string::npos ? slash : slash - pos);
        if (part.empty() || part == "." || part == "..")
            reject("mqtt_credentials_directory_invalid");
        Fd next(::openat(current.value, part.c_str(),
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (next.value < 0) reject("mqtt_credentials_directory_unavailable");
        current = std::move(next);
        pos = slash == std::string::npos ? path.size() : slash + 1;
    }
    struct stat st{};
    if (::fstat(current.value, &st) || !S_ISDIR(st.st_mode) ||
        st.st_uid != ::geteuid() || (st.st_mode & 077) != 0 ||
        (st.st_mode & 0700) != 0700)
        reject("mqtt_credentials_directory_permissions");
    return current;
}
bool contentSafe(const std::string& input) {
    return !input.empty() && input.size() <= 256 &&
           std::none_of(input.begin(), input.end(), [](unsigned char ch) {
               return ch < 32 || ch == 127;
           });
}
void validateLogin(const std::string& user, const std::string& password) {
    if (!contentSafe(user) || !contentSafe(password))
        reject("mqtt_credentials_format");
}
MqttLogin readFile(int directory, const std::string& id) {
    const auto name = id + ".cred";
    Fd secret(::openat(directory, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (secret.value < 0) reject("mqtt_credentials_missing");
    struct stat st{};
    if (::fstat(secret.value, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
        st.st_uid != ::geteuid() || (st.st_mode & 077) != 0 ||
        (st.st_mode & 0400) != 0400 || st.st_size < 3 || st.st_size > 4096)
        reject("mqtt_credentials_permissions");
    std::string bytes;
    std::array<char, 256> buffer{};
    for (;;) {
        const ssize_t n = ::read(secret.value, buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) reject("mqtt_credentials_read");
        if (n == 0) break;
        bytes.append(buffer.data(), static_cast<std::size_t>(n));
        if (bytes.size() > 4096) reject("mqtt_credentials_size");
    }
    const auto first = bytes.find('\n');
    if (first == std::string::npos) reject("mqtt_credentials_format");
    const auto second = bytes.find('\n', first + 1);
    if (second == std::string::npos || second != bytes.size() - 1)
        reject("mqtt_credentials_format");
    MqttLogin login{bytes.substr(0, first),
                    bytes.substr(first + 1, second - first - 1)};
    validateLogin(login.user, login.password);
    return login;
}
std::string randomSuffix() {
    std::array<unsigned char, 16> bytes{};
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t got = ::getrandom(bytes.data() + done, bytes.size() - done, 0);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) reject("mqtt_credentials_random_unavailable");
        done += static_cast<std::size_t>(got);
    }
    const char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (auto v : bytes) { result += hex[v >> 4]; result += hex[v & 15]; }
    return result;
}
void durableWrite(int dir, const std::string& id, const std::string& user,
                  const std::string& password, bool replaceExisting) {
    const auto final = id + ".cred";
    // Check the existing target under our private directory. In particular,
    // rotation must not blindly overwrite a malicious symlink or invalid file.
    if (replaceExisting) (void)readFile(dir, id);
    const auto tmp = "." + id + ".tmp." + randomSuffix();
    struct Cleanup {
        int fd;
        std::string path;
        bool pending = true;
        ~Cleanup() { if (pending) (void)::unlinkat(fd, path.c_str(), 0); }
    } cleanup{dir, tmp};
    Fd file(::openat(dir, tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL |
                                   O_NOFOLLOW | O_CLOEXEC, 0600));
    if (file.value < 0) reject("mqtt_credentials_write");
    if (::fchmod(file.value, 0600)) reject("mqtt_credentials_write");
    const std::string body = user + "\n" + password + "\n";
    for (std::size_t pos = 0; pos < body.size();) {
        const auto n = ::write(file.value, body.data() + pos, body.size() - pos);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) reject("mqtt_credentials_write");
        pos += static_cast<std::size_t>(n);
    }
    if (::fsync(file.value)) reject("mqtt_credentials_write");
    // fsync source before the atomic rename. For create, RENAME_NOREPLACE
    // prevents races against another process creating the same ID.
    if (!replaceExisting) {
#ifdef SYS_renameat2
        if (::syscall(SYS_renameat2, dir, tmp.c_str(), dir, final.c_str(), 1) != 0)
            reject(errno == EEXIST ? "mqtt_credentials_exists" : "mqtt_credentials_write");
#else
        reject("mqtt_credentials_atomic_create_unavailable");
#endif
    } else if (::renameat(dir, tmp.c_str(), dir, final.c_str())) {
        reject("mqtt_credentials_write");
    }
    cleanup.pending = false;
    if (::fsync(dir)) reject("mqtt_credentials_durability_uncertain");
}
} // namespace

MqttCredentials::MqttCredentials(std::string directory)
    : directory_(std::move(directory)) {}
void MqttCredentials::validateId(const std::string& id) {
    if (!validId(id)) reject("mqtt_credentials_id_invalid");
}
MqttLogin MqttCredentials::read(const std::string& id) const {
    if (id.empty()) return {}; // Intentionally anonymous external connection.
    validateId(id);
    std::lock_guard<std::mutex> lock(mutex_);
    auto directory = openDirectory(directory_);
    return readFile(directory.value, id);
}
void MqttCredentials::create(const std::string& id, const std::string& username,
                             const std::string& password) {
    validateId(id);
    validateLogin(username, password);
    std::lock_guard<std::mutex> lock(mutex_);
    auto directory = openDirectory(directory_);
    durableWrite(directory.value, id, username, password, false);
}
void MqttCredentials::rotate(const std::string& id, const std::string& username,
                             const std::string& password) {
    validateId(id);
    validateLogin(username, password);
    std::lock_guard<std::mutex> lock(mutex_);
    auto directory = openDirectory(directory_);
    durableWrite(directory.value, id, username, password, true);
}
void MqttCredentials::erase(const std::string& id) {
    validateId(id);
    std::lock_guard<std::mutex> lock(mutex_);
    auto directory = openDirectory(directory_);
    (void)readFile(directory.value, id); // no symlinks, correct owner/mode.
    if (::unlinkat(directory.value, (id + ".cred").c_str(), 0))
        reject("mqtt_credentials_delete");
    if (::fsync(directory.value)) reject("mqtt_credentials_durability_uncertain");
}
std::vector<std::string> MqttCredentials::ids() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto directory = openDirectory(directory_);
    const int dupFd = ::dup(directory.value);
    if (dupFd < 0) reject("mqtt_credentials_list");
    DIR* raw = ::fdopendir(dupFd);
    if (!raw) { ::close(dupFd); reject("mqtt_credentials_list"); }
    std::unique_ptr<DIR, decltype(&::closedir)> cursor(raw, &::closedir);
    std::vector<std::string> result;
    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(cursor.get());
        if (!entry) {
            if (errno) reject("mqtt_credentials_list");
            break;
        }
        const std::string name(entry->d_name);
        if (name.size() <= 5 || name.substr(name.size() - 5) != ".cred") continue;
        const auto id = name.substr(0, name.size() - 5);
        if (!validId(id)) continue;
        struct stat st{};
        if (::fstatat(directory.value, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) ||
            !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
            st.st_uid != ::geteuid() || (st.st_mode & 077) != 0 ||
            (st.st_mode & 0400) != 0400) continue;
        result.push_back(id);
    }
    std::sort(result.begin(), result.end());
    return result;
}
void MqttCredentials::verifyReferences(const config::Catalog& catalog) const {
    for (const auto& connection : catalog->configuration().connections) {
        if (connection.type == config::ExecutorKind::Mqtt && connection.enabled &&
            !connection.mqtt.credentialsId.empty())
            (void)read(connection.mqtt.credentialsId);
    }
}
} // namespace firefly::execution
