#include "firefly/voice/VoiceProcessor.h"
#include <exception>
#include <chrono>
#include <utility>
namespace firefly::voice {
VoiceResult VoiceProcessor::process(const Utterance& u) {
    VoiceResult r;
    const auto start = std::chrono::steady_clock::now();
    const auto finish = [&] { r.processingMs = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now() - start).count(); return r; };
    r.durationSeconds = double(u.audio.size()) / SampleRate;
    r.speechSeconds = double(u.speech.end - u.speech.start) / SampleRate;
    try {
        if (!u.error.empty()) { r.error = u.error; return finish(); }
        if (u.audio.empty()) { r.error = "Empty utterance"; return finish(); }
        r.recognition = recognizer_.recognize(u.audio);
        if (!r.recognition.success) { r.recognition.text.clear(); r.error = r.recognition.error; return finish(); }
        auto command = handler_(r.recognition.text);
        r.llmTimings = command.llmTimings;
        r.action = std::move(command.action); r.error = command.error;
        r.route = command.path == CommandPath::FastPath ? "FAST_PATH" :
                  command.path == CommandPath::LlmFallback ? "LLM_FALLBACK" : "INPUT_REJECTED";
    } catch (const std::exception& e) { r.action = actions::Unsupported{}; r.recognition.text.clear(); r.error = e.what(); }
    return finish();
}
}
