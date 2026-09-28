#pragma once
#include "firefly/voice/Speech.h"
#include <memory>
#include <stdexcept>
namespace firefly::voice {
class AudioDiscontinuity : public std::runtime_error { using std::runtime_error::runtime_error; };
class AlsaAudioSource final : public AudioSource {
public:
    explicit AlsaAudioSource(const std::string& device, unsigned channels = 1, unsigned channel = 0);
    ~AlsaAudioSource() override;
    int read(float* samples, int capacity) override;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
}
