#include "firefly/voice/VoiceRecovery.h"
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace firefly::voice {
VoiceRecovery::VoiceRecovery() : VoiceRecovery(Timing{}) {}
VoiceRecovery::VoiceRecovery(Timing timing) : timing_(std::move(timing)) {
    if (!timing_.wait) timing_.wait = [](Milliseconds delay) { std::this_thread::sleep_for(delay); };
}
void VoiceRecovery::drain() {
    std::unique_lock<std::mutex> lock(audioMutex_);
    if (!audioChanged_.wait_for(lock, std::chrono::seconds(45), [&] { return !audioActive_; }))
        throw std::runtime_error("audio_drain_timeout");
}
void VoiceRecovery::releaseAudio() {
    { std::lock_guard<std::mutex> lock(audioMutex_); audioActive_ = false; }
    audioChanged_.notify_all();
}
void VoiceRecovery::run(const Hooks& hooks) {
    constexpr Milliseconds poll{50}, initialDelay{1000}, maximumDelay{10000};
    constexpr auto stableSession = std::chrono::seconds(10);
    auto delay = initialDelay;
    auto retryAt = timing_.now();
    bool recovering = false;
    unsigned long long attempt = 0;
    while (!hooks.stopped()) {
        if (hooks.setupBlocked()) { timing_.wait(poll); continue; }
        const auto remaining = retryAt - timing_.now();
        if (remaining > Clock::duration::zero()) {
            timing_.wait(std::min(poll, std::chrono::ceil<Milliseconds>(remaining)));
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(audioMutex_);
            if (hooks.stopped() || hooks.setupBlocked()) continue;
            audioActive_ = true;
        }
        std::optional<Clock::time_point> healthySince;
        std::string failure;
        {
            struct AudioLease {
                VoiceRecovery& owner;
                ~AudioLease() { owner.releaseAudio(); }
            } lease{*this};
            try {
                if (recovering) hooks.journal("retry attempt=" + std::to_string(++attempt));
                hooks.starting();
                hooks.attempt([&] {
                    if (healthySince) return;
                    healthySince = timing_.now();
                    if (recovering) hooks.journal("recovered attempt=" + std::to_string(attempt));
                    recovering = false;
                });
                if (!hooks.stopped() && !hooks.setupBlocked()) failure = "Voice session ended unexpectedly";
            } catch (const std::exception& error) {
                failure = error.what();
                if (failure.empty()) failure = "Voice infrastructure exception";
            }
        } // attempt locals/async workers are gone BEFORE audioActive becomes false.
        if (hooks.stopped()) break;
        if (healthySince && timing_.now() - *healthySince >= stableSession) {
            delay = initialDelay;
            attempt = 0;
        }
        // Intentional setup drain is not an outage. No audio is reopened until
        // setup releases its block, and an existing retry deadline is preserved.
        if (hooks.setupBlocked()) continue;
        if (failure.empty()) continue;
        hooks.failed(failure);
        hooks.journal("failure reason=" + failure);
        recovering = true;
        retryAt = timing_.now() + delay;
        hooks.journal("scheduled delay_ms=" + std::to_string(delay.count()));
        delay = std::min(delay * 2, maximumDelay);
    }
}
}
