#pragma once
#include "firefly/voice/Speech.h"
#include <string_view>
namespace firefly::voice {
struct WakeConfirmationResult {
    bool accepted = false;
    std::string reason;
    std::vector<RecognitionResult> attempts;
    double processingMs = 0;
};
// Secondary, opt-in wake admission only. Never calls a command handler or VAD.
// Reuses the idle command recognizer; its owner must serialize its use/lifetime.
class SttWakeConfirmation {
public:
    explicit SttWakeConfirmation(SpeechRecognizer& recognizer) : recognizer_(recognizer) {}
    WakeConfirmationResult confirm(const std::vector<float>& pcm);
    static bool matches(std::string_view text);
private:
    SpeechRecognizer& recognizer_;
};
}
