#pragma once
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>

namespace firefly::voice {
// One service thread runs attempts synchronously. An attempt owns all capture,
// wake and command workers until it returns/throws; drain waits for their teardown.
class VoiceRecovery {
public:
    using Clock = std::chrono::steady_clock;
    using Milliseconds = std::chrono::milliseconds;
    struct Hooks {
        std::function<bool()> stopped;
        std::function<bool()> setupBlocked;
        std::function<void()> starting;
        std::function<void(const std::function<void()>& ready)> attempt;
        std::function<void(const std::string&)> failed;
        std::function<void(const std::string&)> journal;
    };
    struct Timing {
        std::function<Clock::time_point()> now = Clock::now;
        std::function<void(Milliseconds)> wait;
    };
    VoiceRecovery();
    explicit VoiceRecovery(Timing timing);
    void run(const Hooks& hooks);
    // Caller closes setup admission BEFORE drain. The same mutex protects the
    // blocked check + audioActive transition, so setup cannot miss a new owner.
    void drain();
private:
    void releaseAudio();
    Timing timing_;
    std::mutex audioMutex_;
    std::condition_variable audioChanged_;
    bool audioActive_ = false;
};
}
