#pragma once
#include "firefly/admin/RuntimeWorker.h"
#include <string>
#include <sys/types.h>

namespace firefly::admin {
// Private socketpair framing. No HTTP-supplied executable, path or descriptor.
std::string receiveMessage(int fd, int timeoutSeconds);
void sendMessage(int fd, const std::string& message, int timeoutSeconds);
class ChildRuntime final : public RuntimeClient {
public:
    ChildRuntime(const std::string& executable, const std::string& model);
    ~ChildRuntime() override;
    std::string call(std::string_view request) override;
    ChildRuntime(const ChildRuntime&) = delete;
    ChildRuntime& operator=(const ChildRuntime&) = delete;
private:
    int fd_ = -1;
    pid_t pid_ = -1;
};
}
