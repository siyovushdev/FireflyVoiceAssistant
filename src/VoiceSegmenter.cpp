#include "firefly/voice/VoiceSegmenter.h"
#include <algorithm>
#include <stdexcept>
namespace firefly::voice {
VoiceSegmenter::VoiceSegmenter(SpeechDetector& d, SegmentConfig c) : detector_(d), config_(c) {
    if (c.preMs < 0 || c.preMs > 1000 || c.postMs < 0 || c.postMs > 1000 ||
        c.maxSpeechMs < 1000 || c.maxSpeechMs > 15000)
        throw std::invalid_argument("Padding must be 0..1000 ms; maximum speech 1000..15000 ms");
}
void VoiceSegmenter::reset(bool quarantine) {
    speechSeen_ = false; detector_.reset(); history_.clear(); pending_.clear();
    received_ = base_ = quiet_ = 0; activeStart_ = -1; active_ = false; discarding_ = quarantine;
}
void VoiceSegmenter::startSession() { reset(false); }
void VoiceSegmenter::discontinuity() { reset(true); }
std::vector<Utterance> VoiceSegmenter::accept(const float* samples, int count) {
    if (count <= 0 || count > FrameSamples) throw std::invalid_argument("Invalid audio block");
    history_.insert(history_.end(), samples, samples + count); received_ += count;
    auto detection = detector_.accept(samples, count);
    std::vector<Utterance> out;
    active_ = detection.active;
    speechSeen_ = speechSeen_ || active_ || !detection.completed.empty();
    if (active_ && activeStart_ < 0) activeStart_ = received_ - count;
    if (discarding_) {
        quiet_ = active_ || !detection.completed.empty() ? 0 : quiet_ + count;
        // Vendor max-duration splits are shorter than this guard. Never accept a tail
        // of an oversized/discontinuous utterance as an independent command.
        if (quiet_ >= SampleRate) reset(false);
    } else {
        const auto limit = int64_t(config_.maxSpeechMs) * 16;
        bool tooLong = active_ && activeStart_ >= 0 && received_ - activeStart_ > limit;
        for (auto b : detection.completed) {
            if (b.start < 0 || b.end <= b.start || b.end > received_)
                throw std::runtime_error("Invalid VAD boundary");
            tooLong = tooLong || b.end - b.start > limit;
        }
        if (tooLong) {
            out.push_back({{}, {}, "Maximum speech duration exceeded; discarding until silence"});
            pending_.clear(); discarding_ = true; quiet_ = 0;
        } else {
            for (auto b : detection.completed) pending_.push_back(b);
            while (!pending_.empty() && received_ >= pending_.front().end + config_.postMs * 16) {
                auto b = pending_.front(); pending_.pop_front();
                auto first = std::max<int64_t>(0, b.start - config_.preMs * 16);
                auto last = b.end + config_.postMs * 16;
                if (first < base_) out.push_back({b, {}, "Incomplete audio history"});
                else out.push_back({b, {history_.begin() + (first - base_), history_.begin() + (last - base_)}, {}});
            }
        }
        if (!active_) activeStart_ = -1;
    }
    const size_t capacity = (config_.maxSpeechMs + 4000) * 16;
    while (history_.size() > capacity) { history_.pop_front(); ++base_; }
    // Stop before vendor int32 sample coordinates can overflow; never reset mid-prefix.
    if (received_ > int64_t(SampleRate) * 24 * 3600)
        throw std::runtime_error("Voice session reached 24 hours; restart required");
    return out;
}
}
