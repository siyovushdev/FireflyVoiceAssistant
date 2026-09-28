#pragma once
#include "firefly/voice/WakeDetector.h"
#include <memory>
namespace firefly::voice {
// Bounded local IPC to a PCM-only LiteRT worker. Owns and reaps exactly one child.
class AminaWakeDetector final : public WakeDetector {
public:
    AminaWakeDetector(const WakeConfig& config, const std::string& python, const std::string& worker);
    ~AminaWakeDetector() override;
    bool accept(const float* samples, int count) override;
    void reset() override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
