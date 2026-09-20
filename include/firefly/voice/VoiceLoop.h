#pragma once
#include "firefly/voice/VoiceProcessor.h"
#include <functional>
namespace firefly::voice {
// Shared capture orchestration: one persistent source, no queued busy speech,
// joined processing worker and quiet rearm. Owners supply their command route.
void runVoiceLoop(AudioSource& capture, VoiceSegmenter& segmenter,
    const std::function<VoiceResult(const Utterance&)>& process,
    const std::function<void(const VoiceResult&)>& emit,
    const std::function<void(const std::string&)>& state,
    const std::function<bool()>& stopped);
}
