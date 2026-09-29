#pragma once
#include "firefly/voice/Speech.h"
#include <algorithm>
#include <array>
namespace firefly::voice {
// Only the current wake admission can consume this history. Never a command queue.
class WakePcmRing {
public:
    static constexpr size_t Capacity = 3 * SampleRate;
    void clear() { next_ = size_ = 0; }
    void append(const float* samples, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            data_[next_] = samples[i]; next_ = (next_ + 1) % Capacity;
            size_ = std::min(size_ + 1, Capacity);
        }
    }
    size_t size() const { return size_; }
    std::vector<float> snapshot() const {
        std::vector<float> result; result.reserve(size_);
        const auto first = (next_ + Capacity - size_) % Capacity;
        for (size_t i = 0; i < size_; ++i) result.push_back(data_[(first + i) % Capacity]);
        return result;
    }
private:
    std::array<float, Capacity> data_{};
    size_t next_ = 0, size_ = 0;
};
}
