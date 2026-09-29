#include "firefly/voice/VoiceProcessor.h"
#include <exception>
#include <chrono>
#include <utility>
#include <optional>
namespace firefly::voice {
namespace {
std::optional<std::string> inlineCommand(const std::string& text) {
    // Exact, documented STT prefixes only. No fuzzy matching or transcript repair.
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::nullopt;
    for (const std::string prefix : {"амина", "Амина", "АМИНА", "афина", "Афина", "АФИНА"}) {
        if (text.compare(first, prefix.size(), prefix) != 0) continue;
        auto end = first + prefix.size();
        if (end == text.size() || std::string(" ,:!.?;\t\r\n").find(text[end]) == std::string::npos)
            return std::nullopt;
        end = text.find_first_not_of(" ,:!.?;\t\r\n", end);
        if (end == std::string::npos) return std::nullopt;
        return text.substr(end);
    }
    return std::nullopt;
}
}
VoiceResult VoiceProcessor::process(const Utterance& u) {
    VoiceResult r; r.inlineWake = u.inlineWake;
    const auto start = std::chrono::steady_clock::now();
    const auto finish = [&] { r.processingMs = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now() - start).count(); return r; };
    r.durationSeconds = double(u.audio.size()) / SampleRate;
    r.speechSeconds = double(u.speech.end - u.speech.start) / SampleRate;
    try {
        if (!u.error.empty()) { r.error = u.error; return finish(); }
        if (u.audio.empty()) { r.error = "Empty utterance"; return finish(); }
        r.recognition = recognizer_.recognize(u.audio);
        if (!r.recognition.success) { r.recognition.text.clear(); r.error = r.recognition.error; return finish(); }
        r.commandText = r.recognition.text;
        if (u.inlineWake) {
            auto commandText = inlineCommand(r.recognition.text);
            if (!commandText) { r.error = "Unrecognized or empty leading wake invocation"; return finish(); }
            r.commandText = std::move(*commandText); r.wakePrefixRemoved = true;
        }
        auto command = handler_(r.commandText);
        r.llmTimings = command.llmTimings;
        r.action = std::move(command.action); r.error = command.error;
        r.route = command.path == CommandPath::FastPath ? "FAST_PATH" :
                  command.path == CommandPath::LlmFallback ? "LLM_FALLBACK" : "INPUT_REJECTED";
    } catch (const std::exception& e) { r.action = actions::Unsupported{}; r.recognition.text.clear(); r.error = e.what(); }
    return finish();
}
}
