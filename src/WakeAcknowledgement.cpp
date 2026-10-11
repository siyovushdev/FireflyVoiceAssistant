#include "firefly/voice/WakeAcknowledgement.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace firefly::voice {
void WakeAcknowledgementConfig::validate() const {
    if (volumePercent < 1 || volumePercent > 30)
        throw std::invalid_argument("Wake beep volume must be an integer in 1..30 percent");
}

std::vector<int16_t> makeWakeAcknowledgementPcm(const WakeAcknowledgementConfig& config) {
    config.validate();
    if (!config.enabled) return {};
    constexpr size_t SampleCount = 6400;
    constexpr double FadeSamples = 160;
    constexpr double Pi = 3.141592653589793;
    std::vector<int16_t> samples(SampleCount);
    const double amplitude = 32767.0 * config.volumePercent / 100.0;
    for (size_t i = 0; i < samples.size(); ++i) {
        const double fade = std::min({1.0, i / FadeSamples, (samples.size() - 1 - i) / FadeSamples});
        samples[i] = static_cast<int16_t>(amplitude * fade * std::sin(2 * Pi * 880 * i / 16000));
    }
    return samples;
}
}
