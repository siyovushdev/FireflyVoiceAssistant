#pragma once
#include <string>
#include <string_view>
#include <sys/un.h>

namespace firefly::voice {
// Optional operator-controlled beam controller. Sends lifecycle metadata only;
// no audio, transcript, model facts or device actions. No helper is started here.
class WakeDirectionClient final {
public:
    explicit WakeDirectionClient(const std::string& socketPath = {});
    ~WakeDirectionClient();
    WakeDirectionClient(const WakeDirectionClient&) = delete;
    WakeDirectionClient& operator=(const WakeDirectionClient&) = delete;
    bool enabled() const noexcept { return !path_.empty(); }
    // Nonblocking datagram: missing/full controller never stalls audio capture.
    bool notify(std::string_view state) const noexcept;
private:
    std::string path_;
    sockaddr_un address_{};
    int descriptor_ = -1;
};
}
