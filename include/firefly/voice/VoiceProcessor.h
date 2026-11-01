#pragma once
#include "firefly/voice/VoiceSegmenter.h"
#include "firefly/CommandEngine.h"
#include <functional>
namespace firefly::voice {
struct VoiceResult {
    RecognitionResult recognition;
    bool inlineWake = false, wakePrefixRemoved = false;
    std::string commandText;
    // Grounded operational response; never model-generated device facts.
    std::string replyText, replyKind, replyReason, replyMode = "off";
    std::uint64_t replyGeneration = 0;
    actions::CandidateResult action;
    std::string route = "NOT_CALLED", error;
    CommandTimings llmTimings;
    double processingMs = 0;
    double durationSeconds = 0, speechSeconds = 0;
};
class VoiceProcessor {
public:
    using CommandHandler = std::function<CommandResult(std::string_view)>;
    VoiceProcessor(SpeechRecognizer& recognizer, CommandEngine& engine)
        : VoiceProcessor(recognizer, [&engine](std::string_view text) { return engine.process(text); }) {}
    VoiceProcessor(SpeechRecognizer& recognizer, CommandHandler handler) : recognizer_(recognizer), handler_(std::move(handler)) {}
    VoiceResult process(const Utterance& utterance);
private:
    SpeechRecognizer& recognizer_;
    CommandHandler handler_;
};
}
