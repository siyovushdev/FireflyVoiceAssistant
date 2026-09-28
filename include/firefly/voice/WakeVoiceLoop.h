#pragma once
#include "firefly/voice/VoiceLoop.h"
#include "firefly/voice/WakeDetector.h"
namespace firefly::voice {
// Single audio owner. Only WAITING_WAKE feeds KWS; only LISTENING_COMMAND feeds VAD.
// PROCESSING drains/discards capture, with one joined worker and no queued speech.
// Any capture/KWS/VAD exception closes admission and propagates to subsystem health.
void runWakeVoiceLoop(AudioSource& capture, WakeDetector& wake, VoiceSegmenter& segmenter,
    const WakeConfig& config,
    const std::function<VoiceResult(const Utterance&)>& process,
    const std::function<void(const VoiceResult&)>& emit,
    const std::function<void(const std::string&)>& state,
    const std::function<bool()>& stopped,
    const std::function<std::chrono::steady_clock::time_point()>& now = std::chrono::steady_clock::now);
}
