#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace firefly::voice {
constexpr int SampleRate = 16000;
constexpr int FrameSamples = 512;
struct SpeechBoundary { int64_t start; int64_t end; };
struct Detection { bool active = false; std::vector<SpeechBoundary> completed; };
class SpeechDetector {
public:
    virtual ~SpeechDetector() = default;
    virtual Detection accept(const float* samples, int count) = 0;
    virtual void reset() = 0;
    // Confirmation delay inherent in the unchanged detector, not a VAD tuning knob.
    virtual int startConfirmationMs() const { return 0; }
};
struct RecognitionResult {
    bool success = false;
    std::string text;
    std::string error;
    double latencyMs = 0;
};
class SpeechRecognizer {
public:
    virtual ~SpeechRecognizer() = default;
    virtual RecognitionResult recognize(const std::vector<float>& audio) = 0;
};
class AudioSource {
public:
    virtual ~AudioSource() = default;
    // Returns 0 on a bounded wait timeout. PCM is normalized without resampling here.
    virtual int read(float* samples, int capacity) = 0;
};
}
