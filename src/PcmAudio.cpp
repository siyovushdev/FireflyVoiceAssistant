#include "firefly/voice/PcmAudio.h"
#include <limits>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace firefly::voice {
namespace {
std::uint16_t le16(const unsigned char* p) {
    return static_cast<std::uint16_t>(p[0] | (std::uint16_t(p[1]) << 8));
}
std::uint32_t le32(const unsigned char* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
bool tagEq(const unsigned char* p, const char* tag) {
    return std::memcmp(p, tag, 4) == 0;
}
std::vector<float> resampleLinear(const std::vector<float>& input, int fromRate, int toRate) {
    if (fromRate == toRate) return input;
    if (input.empty()) return {};
    const auto outCount = static_cast<std::size_t>((double(input.size()) * toRate) / fromRate);
    if (outCount == 0) return {};
    std::vector<float> out(outCount);
    const double scale = double(fromRate) / toRate;
    for (std::size_t i = 0; i < outCount; ++i) {
        const double src = i * scale;
        const auto left = static_cast<std::size_t>(src);
        const auto right = std::min(left + 1, input.size() - 1);
        const float frac = static_cast<float>(src - left);
        out[i] = input[left] * (1.f - frac) + input[right] * frac;
    }
    return out;
}
}

DecodedPcm decodeWavToSttPcm(std::string_view bytes, int maximumSeconds) {
    if (maximumSeconds != 15 && maximumSeconds != 60) throw std::invalid_argument("Invalid audio profile");
    if (bytes.empty() || bytes.size() > static_cast<std::size_t>(maximumSeconds)*48000*2+256)
        throw std::invalid_argument("WAV body empty or exceeds Admin Copilot upload limit");
    if (bytes.size() < 44) throw std::invalid_argument("WAV too small");
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    if (!tagEq(data, "RIFF") || !tagEq(data + 8, "WAVE")) throw std::invalid_argument("Not a RIFF/WAVE file");

    std::size_t offset = 12;
    bool haveFmt = false, haveData = false;
    std::uint16_t audioFormat = 0, channels = 0, bits = 0;
    std::uint32_t sampleRate = 0;
    const unsigned char* pcm = nullptr;
    std::uint32_t pcmBytes = 0;

    while (offset + 8 <= bytes.size()) {
        const auto* chunk = data + offset;
        const auto size = le32(chunk + 4);
        const auto next = offset + 8 + size + (size & 1u);
        if (next > bytes.size()) throw std::invalid_argument("Truncated WAV chunk");
        if (tagEq(chunk, "fmt ")) {
            if (size < 16) throw std::invalid_argument("Invalid WAV fmt chunk");
            audioFormat = le16(chunk + 8);
            channels = le16(chunk + 10);
            sampleRate = le32(chunk + 12);
            bits = le16(chunk + 22);
            haveFmt = true;
        } else if (tagEq(chunk, "data")) {
            pcm = chunk + 8;
            pcmBytes = size;
            haveData = true;
        }
        offset = next;
        if (haveFmt && haveData) break;
    }
    if (!haveFmt || !haveData || !pcm) throw std::invalid_argument("WAV missing fmt/data");
    if (audioFormat != 1) throw std::invalid_argument("Only PCM WAV is supported");
    if (channels != 1) throw std::invalid_argument("Only mono WAV is supported");
    if (bits != 16) throw std::invalid_argument("Only 16-bit PCM WAV is supported");
    if (sampleRate != 8000 && sampleRate != 16000 && sampleRate != 22050 && sampleRate != 44100 && sampleRate != 48000)
        throw std::invalid_argument("Unsupported WAV sample rate");
    if (pcmBytes == 0 || (pcmBytes % 2) != 0) throw std::invalid_argument("Invalid WAV data size");

    const auto frames = pcmBytes / 2;
    const double durationMs = double(frames) * 1000.0 / sampleRate;
    if (durationMs > maximumSeconds * 1000.0 + 0.5)
        throw std::invalid_argument("Recording exceeds " + std::to_string(maximumSeconds) + " second Admin recording limit");

    std::vector<float> mono;
    mono.reserve(frames);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const auto sample = static_cast<std::int16_t>(le16(pcm + i * 2));
        mono.push_back(float(sample) / 32768.f);
    }
    auto stt = resampleLinear(mono, static_cast<int>(sampleRate), SampleRate);
    if (stt.empty()) throw std::invalid_argument("Decoded audio is empty");
    if (stt.size() > std::size_t(maximumSeconds) * SampleRate)
        throw std::invalid_argument("Resampled audio exceeds STT utterance bound");
    for (float sample : stt)
        if (!std::isfinite(sample)) throw std::invalid_argument("Non-finite PCM sample");

    DecodedPcm decoded;
    decoded.samples = std::move(stt);
    decoded.sourceRate = static_cast<int>(sampleRate);
    decoded.durationMs = durationMs;
    return decoded;
}
std::vector<SpeechBoundary> adminRecordingSegments(const std::vector<float>& samples) {
    if (samples.empty() || samples.size() > 60 * SampleRate)
        throw std::invalid_argument("Invalid Admin recording duration");
    std::vector<SpeechBoundary> segments;
    std::size_t offset = 0;
    while (offset < samples.size()) {
        std::size_t end = samples.size();
        if (end - offset > 17 * SampleRate) {
            end = offset + 15 * SampleRate;
            double lowestEnergy = std::numeric_limits<double>::max();
            constexpr std::size_t window = SampleRate / 20;
            for (std::size_t candidate = offset + 12 * SampleRate;
                 candidate + window <= offset + 15 * SampleRate; candidate += window) {
                double energy = 0;
                for (std::size_t i = candidate; i < candidate + window; ++i)
                    energy += samples[i] * samples[i];
                if (energy < lowestEnergy) {
                    lowestEnergy = energy;
                    end = candidate + window / 2;
                }
            }
        }
        segments.push_back({static_cast<int64_t>(offset), static_cast<int64_t>(end)});
        offset = end;
    }
    return segments;
}

}
