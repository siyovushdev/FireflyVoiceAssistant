#pragma once
#include "firefly/voice/Speech.h"
#include <deque>
namespace firefly::voice {
struct SegmentConfig { int preMs = 300; int postMs = 300; int maxSpeechMs = 15000; };
struct Utterance {
    SpeechBoundary speech{};
    std::vector<float> audio;
    std::string error;
};
// Bounded PCM history, independent of ALSA, sherpa, and command semantics.
class VoiceSegmenter {
public:
    VoiceSegmenter(SpeechDetector& detector, SegmentConfig config = {});
    std::vector<Utterance> accept(const float* samples, int count);
    void discontinuity();
    // Begin a newly admitted wake session with fresh VAD coordinates and no history.
    void startSession();
    bool active() const { return active_ || !pending_.empty() || discarding_; }
    bool speechSeen() const { return speechSeen_; }
    int startConfirmationMs() const { return detector_.startConfirmationMs(); }
    int maxSpeechMs() const { return config_.maxSpeechMs; }
    size_t bufferedSamples() const { return history_.size(); }
private:
    void reset(bool quarantine);
    SpeechDetector& detector_;
    SegmentConfig config_;
    std::deque<float> history_;
    std::deque<SpeechBoundary> pending_;
    int64_t received_ = 0, base_ = 0, activeStart_ = -1, quiet_ = 0;
    bool active_ = false, discarding_ = false, speechSeen_ = false;
};
}
