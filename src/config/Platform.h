#pragma once
#include <string>
#include <string_view>

namespace firefly::config::detail {
class FileDescriptor final {
public:
    explicit FileDescriptor(int value = -1) : value_(value) {}
    ~FileDescriptor();
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept;
    FileDescriptor& operator=(FileDescriptor&& other) noexcept;
    int get() const { return value_; }
private:
    int value_;
};
[[noreturn]] void ioError(const std::string& operation);
FileDescriptor openDirectory(const std::string& path);
std::string readDescriptor(int file);
std::string readAt(int directory, const std::string& name);
std::string readFile(const std::string& path);
std::string sha256(std::string_view bytes);
std::string lowercase(std::string text);
void checkText(std::string_view text, const std::string& path);
}
