#include "firefly/voice/WakeAcknowledgement.h"
#include <alsa/asoundlib.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
namespace firefly::voice {
struct AlsaWakeAcknowledgement::Impl {
    std::string device, failure;
    snd_pcm_t* pcm = nullptr;
    std::array<int16_t, 1600> samples{}; // 100 ms, 16 kHz, mono.
    size_t written = 0;
    bool started = false;
    std::chrono::steady_clock::time_point deadline;
    void close() noexcept {
        if (pcm) { snd_pcm_drop(pcm); snd_pcm_close(pcm); pcm = nullptr; }
    }
    void fail(const std::string& message) { failure = message; close(); }
    ~Impl() { close(); }
};
AlsaWakeAcknowledgement::AlsaWakeAcknowledgement(std::string device) : impl_(std::make_unique<Impl>()) {
    impl_->device = std::move(device);
    for (size_t i = 0; i < impl_->samples.size(); ++i) {
        const double fade = std::min({1.0, double(i) / 160, double(impl_->samples.size() - 1 - i) / 160});
        impl_->samples[i] = static_cast<int16_t>(32767 * .12 * fade * std::sin(2 * 3.141592653589793 * 880 * i / 16000));
    }
}
AlsaWakeAcknowledgement::~AlsaWakeAcknowledgement() = default;
void AlsaWakeAcknowledgement::cancel() noexcept { impl_->close(); }
std::string AlsaWakeAcknowledgement::error() const { return impl_->failure; }
void AlsaWakeAcknowledgement::start() {
    auto& p = *impl_;
    p.close(); p.failure.clear(); p.written = 0; p.started = false;
    p.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    int rc = snd_pcm_open(&p.pcm, p.device.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (rc >= 0) rc = snd_pcm_set_params(p.pcm, SND_PCM_FORMAT_S16_LE,
        SND_PCM_ACCESS_RW_INTERLEAVED, 1, 16000, 1, 100000);
    if (rc < 0) p.fail(std::string("ALSA acknowledgement setup: ") + snd_strerror(rc));
}
AcknowledgementStatus AlsaWakeAcknowledgement::poll() {
    auto& p = *impl_;
    if (!p.failure.empty()) return AcknowledgementStatus::Failed;
    if (!p.pcm) return AcknowledgementStatus::Complete;
    if (std::chrono::steady_clock::now() >= p.deadline) {
        p.fail("ALSA acknowledgement deadline exceeded"); return AcknowledgementStatus::Failed;
    }
    if (p.written < p.samples.size()) {
        const auto n = snd_pcm_writei(p.pcm, p.samples.data() + p.written, p.samples.size() - p.written);
        if (n == -EAGAIN) return AcknowledgementStatus::Playing;
        if (n < 0) { p.fail(std::string("ALSA acknowledgement write: ") + snd_strerror(n)); return AcknowledgementStatus::Failed; }
        p.written += static_cast<size_t>(n);
    }
    if (p.written == p.samples.size()) {
        if (!p.started) {
            if (snd_pcm_state(p.pcm) == SND_PCM_STATE_PREPARED) {
                const int rc = snd_pcm_start(p.pcm);
                if (rc < 0) { p.fail(std::string("ALSA acknowledgement start: ") + snd_strerror(rc)); return AcknowledgementStatus::Failed; }
            }
            p.started = true;
        }
        // Nonblocking drain reports completion without cutting off the last frame.
        const int rc = snd_pcm_drain(p.pcm);
        if (rc == -EAGAIN) return AcknowledgementStatus::Playing;
        if (rc < 0) { p.fail(std::string("ALSA acknowledgement drain: ") + snd_strerror(rc)); return AcknowledgementStatus::Failed; }
        p.close(); return AcknowledgementStatus::Complete;
    }
    return AcknowledgementStatus::Playing;
}
}
