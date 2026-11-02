#pragma once
#include <chrono>
#include <string>
namespace firefly::voice {
struct WakeConfig {
    bool enabled = true;
    std::string modelPath;
    double cutoff = 0.99;
    int window = 7;
    // Programmatic/replay compatibility; the appliance CLI defaults this to true.
    bool afterBeepOnly = false;
    // Explicit legacy option retains its TOTAL listening deadline; zero disables it.
    std::chrono::milliseconds commandTimeout{0};
    std::chrono::milliseconds commandStartTimeout{5000};
    // Drain capture buffer + short acoustic tail after playback, before fresh VAD.
    std::chrono::milliseconds acknowledgementDrain{250};
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
