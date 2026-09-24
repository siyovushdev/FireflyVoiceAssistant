#pragma once
#include "firefly/voice/Speech.h"
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace firefly::voice {
// Admin Copilot / browser upload utterance bound (matches live --max-utterance-ms default).
inline constexpr int MaxUtteranceSeconds = 15;
// 15 s mono S16 at 48 kHz plus a small RIFF header budget.
inline constexpr std::size_t MaxWavBytes = 15 * 48000 * 2 + 256;

struct DecodedPcm {
    std::vector<float> samples; // mono, SampleRate, finite [-1, 1]
    int sourceRate = 0;
    double durationMs = 0;
};

// Strict PCM WAV → STT float PCM. Rejects stereo, compressed codecs, and unknown rates.
// Resamples 8/16/22.05/44.1/48 kHz to SampleRate with linear interpolation.
DecodedPcm decodeWavToSttPcm(std::string_view bytes);
}
