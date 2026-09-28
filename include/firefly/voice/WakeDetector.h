#pragma once
#include <chrono>
#include <string>
namespace firefly::voice {
struct WakeConfig {
    bool enabled = true;
    std::string modelPath;
    double cutoff = 0.99;
    int window = 7;
    std::chrono::milliseconds commandTimeout{10000};
    void validate() const;
};
class WakeDetector {
public:
    virtual ~WakeDetector() = default;
    virtual bool accept(const float* samples, int count) = 0;
    // Reset frontend, smoothing history AND streaming model state before rearming.
    virtual void reset() = 0;
};
}
