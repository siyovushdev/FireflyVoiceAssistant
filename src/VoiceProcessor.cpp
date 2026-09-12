#include "firefly/voice/VoiceProcessor.h"
#include <exception>
namespace firefly::voice {
VoiceResult VoiceProcessor::process(const Utterance& u) {
    VoiceResult r;
    r.durationSeconds = double(u.audio.size()) / SampleRate;
    r.speechSeconds = double(u.speech.end - u.speech.start) / SampleRate;
    try {
        if (!u.error.empty()) { r.error = u.error; return r; }
        if (u.audio.empty()) { r.error = "Empty utterance"; return r; }
        r.recognition = recognizer_.recognize(u.audio);
        if (!r.recognition.success) { r.recognition.text.clear(); r.error = r.recognition.error; return r; }
        auto command = engine_.process(r.recognition.text);
        r.action = command.compact(); r.error = command.error;
        r.route = command.path == CommandPath::FastPath ? "FAST_PATH" :
                  command.path == CommandPath::LlmFallback ? "LLM_FALLBACK" : "INPUT_REJECTED";
    } catch (const std::exception& e) { r.action = "U"; r.recognition.text.clear(); r.error = e.what(); }
    return r;
}
}
