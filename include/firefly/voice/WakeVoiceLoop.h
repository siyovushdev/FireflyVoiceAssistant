#pragma once
#include "firefly/voice/VoiceLoop.h"
#include "firefly/voice/WakeDetector.h"
#include "firefly/voice/WakeAcknowledgement.h"
namespace firefly::voice {
// Single capture owner. WAITING_WAKE feeds KWS; WaitingSpeech/Capturing feed VAD.
// Acknowledging/draining/processing discard PCM. One joined command worker, no queue.
// Any capture/KWS/VAD exception closes admission and propagates to subsystem health.
void runWakeVoiceLoop(AudioSource& capture, WakeDetector& wake, VoiceSegmenter& segmenter,
    const WakeConfig& config,
    const std::function<VoiceResult(const Utterance&)>& process,
    const std::function<void(const VoiceResult&)>& emit,
    const std::function<void(const std::string&)>& state,
    const std::function<bool()>& stopped,
    const std::function<std::chrono::steady_clock::time_point()>& now = std::chrono::steady_clock::now,
    WakeAcknowledgement* acknowledgement = nullptr,
    const std::function<bool(const std::vector<float>&)>& confirmWake = {});
}
