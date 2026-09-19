#pragma once
#include "firefly/voice/VoiceSegmenter.h"
#include "firefly/CommandEngine.h"
namespace firefly::voice {
struct VoiceResult {
    RecognitionResult recognition;
    actions::CandidateResult action;
    std::string route = "NOT_CALLED", error;
    double durationSeconds = 0, speechSeconds = 0;
};
class VoiceProcessor {
public:
    VoiceProcessor(SpeechRecognizer& recognizer, CommandEngine& engine) : recognizer_(recognizer), engine_(engine) {}
    VoiceResult process(const Utterance& utterance);
private:
    SpeechRecognizer& recognizer_;
    CommandEngine& engine_;
};
}
