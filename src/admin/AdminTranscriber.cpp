#include "firefly/admin/AdminTranscriber.h"
#include "firefly/voice/PcmAudio.h"
#include "Json.h"

namespace firefly::admin {
AdminTranscriber::AdminTranscriber(std::unique_ptr<voice::SpeechRecognizer> recognizer,
                                 std::shared_ptr<SetupResources> resources)
    : recognizer_(std::move(recognizer)), resources_(std::move(resources)) {
    if (!recognizer_) throw std::invalid_argument("AdminTranscriber requires a recognizer");
}
bool AdminTranscriber::ready() const { return recognizer_ != nullptr; }

std::string AdminTranscriber::transcribe(std::string_view wavBytes, int maximumSeconds) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return failure("stt_busy", "", "Speech recognition is already running. Wait and retry.").dump();
    if (!recognizer_)
        return failure("stt_unavailable", "", "Local speech recognition is not available on this Firefly.").dump();

    std::unique_lock<std::timed_mutex> resources;
    if (resources_) {
        try { resources = resources_->acquire(std::chrono::milliseconds(0)); }
        catch (...) { return failure("stt_busy", "", "Model resources are changing. Wait and retry.").dump(); }
    }

    voice::DecodedPcm decoded;
    try {
        decoded = voice::decodeWavToSttPcm(wavBytes, maximumSeconds);
    } catch (const std::exception& error) {
        return failure("invalid_audio", "", std::string(error.what())).dump();
    }

    const auto recognition = recognizer_->recognize(decoded.samples);
    if (!recognition.success) {
        // Never surface sherpa/internal paths to the Admin UI.
        return failure("stt_failed", "", "Speech could not be recognized. Try again or type the request.").dump();
    }
    std::string text = recognition.text;
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n' || text.back() == '\r' || text.back() == '\t'))
        text.pop_back();
    std::size_t start = 0;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\n' || text[start] == '\r' || text[start] == '\t'))
        ++start;
    text = text.substr(start);
    if (text.empty())
        return failure("empty_speech", "", "Speech was not detected. Speak a short Admin request and try again.").dump();

    return Json{{"ok", true}, {"text", text}, {"duration_ms", decoded.durationMs},
        {"stt_ms", recognition.latencyMs}, {"segmented", decoded.durationMs > 17000}, {"saved", false}, {"applied", false},
        {"copilot_started", false}}.dump();
}
}
