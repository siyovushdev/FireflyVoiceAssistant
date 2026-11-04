#pragma once
#include <list>
#include <string>
#include <vector>

namespace firefly::voice::f2 {
// One fixed F2 model/profile per Speaker. Only exact, freshly requested text
// selects PCM; this cache owns no device state, history, targets or commands.
// Accessed by the synthesis worker only. Discarded with the audio lease.
class AudioCache {
    struct Entry { std::string text; std::vector<float> pcm; };
    std::list<Entry> entries_;
    std::size_t bytes_ = 0;
public:
    static constexpr std::size_t MaxBytes = 8 * 1024 * 1024;
    static constexpr std::size_t MaxEntries = 16;
    static constexpr std::size_t MaxSamples = 20 * 44100;
    std::vector<float> get(const std::string& text) {
        for (auto i = entries_.begin(); i != entries_.end(); ++i) if (i->text == text) {
            entries_.splice(entries_.begin(), entries_, i);
            return entries_.front().pcm;
        }
        return {};
    }
    void put(const std::string& text, const std::vector<float>& pcm) {
        if (pcm.empty() || pcm.size() > MaxSamples || text.size() > 4096) return;
        for (auto i = entries_.begin(); i != entries_.end(); ++i) if (i->text == text) {
            bytes_ -= i->pcm.size() * sizeof(float) + i->text.size(); entries_.erase(i); break;
        }
        const auto bytes = pcm.size() * sizeof(float) + text.size();
        while (!entries_.empty() && (bytes_ + bytes > MaxBytes || entries_.size() >= MaxEntries)) {
            bytes_ -= entries_.back().pcm.size() * sizeof(float) + entries_.back().text.size(); entries_.pop_back();
        }
        entries_.push_front({text, pcm}); bytes_ += bytes;
    }
    std::size_t bytes() const { return bytes_; }
    std::size_t size() const { return entries_.size(); }
};
}
