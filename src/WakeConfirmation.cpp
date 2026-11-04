#include "firefly/voice/WakeConfirmation.h"
#include "Text.h"
#include <chrono>
#include <cmath>
#include <exception>
#include <stdexcept>
namespace firefly::voice {
bool SttWakeConfirmation::matches(std::string_view text) {
    if (text.size() > 1024) return false;
    const auto words = detail::words(text);
    for (std::size_t i = 0; i < words.size(); ++i) {
        // Owner accepts the established STT alias Afina. This is wake-only:
        // neither the transcript nor command grounding is changed/repaired.
        if (words[i] == "амина" || words[i] == "афина" ||
            (words[i] == "а" && i + 1 < words.size() && words[i + 1] == "мина")) return true;
    }
    return false;
}
WakeConfirmationResult SttWakeConfirmation::confirm(const std::vector<float>& pcm) {
    const auto start = std::chrono::steady_clock::now();
    WakeConfirmationResult result;
    try {
        if (pcm.empty() || pcm.size() > 3 * SampleRate + 8 * FrameSamples) throw std::invalid_argument("Invalid wake history");
        for (float sample : pcm)
            if (!std::isfinite(sample) || sample < -1 || sample > 1) throw std::invalid_argument("Invalid wake PCM");
        // One full context, then one bounded tail if context obscures the name.
        // No gain, VAD, normalization, padding, model reload or transcript repair.
        result.attempts.push_back(recognizer_.recognize(pcm));
        if (!result.attempts.back().success) {
            result.reason = "stt_unavailable";
        } else {
            result.accepted = matches(result.attempts.back().text);
            constexpr std::size_t TailSamples = SampleRate * 1600 / 1000;
            if (!result.accepted && pcm.size() > TailSamples) {
                result.attempts.push_back(recognizer_.recognize({pcm.end() - TailSamples, pcm.end()}));
                if (!result.attempts.back().success) result.reason = "stt_unavailable";
                else result.accepted = matches(result.attempts.back().text);
            }
            if (result.reason.empty()) result.reason = result.accepted ? "name_confirmed" : "name_not_found";
        }
    } catch (const std::exception&) { result.accepted = false; result.reason = "confirmation_failed"; }
    result.processingMs = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now() - start).count();
    return result;
}
}
