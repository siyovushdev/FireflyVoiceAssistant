#include "firefly/voice/AlsaAudioSource.h"
#include <alsa/asoundlib.h>
#include <array>
#include <cerrno>
namespace firefly::voice {
struct AlsaAudioSource::Impl {
    snd_pcm_t* handle = nullptr;
    ~Impl() { if (handle) { snd_pcm_drop(handle); snd_pcm_close(handle); } }
};
AlsaAudioSource::AlsaAudioSource(const std::string& device) : impl_(std::make_unique<Impl>()) {
    int rc = snd_pcm_open(&impl_->handle, device.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if (rc < 0) throw std::runtime_error("Cannot open ALSA capture " + device + ": " + snd_strerror(rc));
    rc = snd_pcm_set_params(impl_->handle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                           1, SampleRate, 1, 200000);
    if (rc < 0) throw std::runtime_error("Cannot configure 16kHz mono S16_LE: " + std::string(snd_strerror(rc)));
}
AlsaAudioSource::~AlsaAudioSource() = default;
int AlsaAudioSource::read(float* samples, int capacity) {
    if (capacity <= 0 || capacity > FrameSamples) throw std::invalid_argument("Invalid capture block");
    std::array<int16_t, FrameSamples> pcm{};
    auto n = snd_pcm_readi(impl_->handle, pcm.data(), capacity);
    if (n == -EAGAIN || n == 0) {
        int rc = snd_pcm_wait(impl_->handle, 50);
        if (rc >= 0 || rc == -EINTR) return 0;
        n = rc;
    }
    if (n == -EINTR) return 0;
    if (n < 0) {
        // Never concatenate audio across an overrun/suspend gap. Avoid snd_pcm_recover's
        // potentially unbounded suspend retry loop; prepare once or report fatal failure.
        if (n == -EPIPE || n == -ESTRPIPE) {
            int rc = snd_pcm_prepare(impl_->handle);
            if (rc >= 0) throw AudioDiscontinuity("ALSA capture discontinuity; utterance discarded");
        }
        throw std::runtime_error("ALSA capture read failed: " + std::string(snd_strerror(n)));
    }
    for (int i = 0; i < n; ++i) samples[i] = pcm[i] / 32768.0f;
    return static_cast<int>(n);
}
}
