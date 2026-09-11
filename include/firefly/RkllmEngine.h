#pragma once
#include "firefly/LlmEngine.h"
#include <functional>
#include <chrono>
#include <memory>
namespace firefly {
struct RkllmConfig {
    std::string modelPath;
    std::string systemPrompt;
    bool verbose = false;
    std::chrono::milliseconds requestTimeout{30000};
    std::function<void(std::string_view)> progress;
};
// Owns one synchronous runtime. Requests are serialized; do not destroy while in use.
class RkllmEngine final : public LlmEngine {
public:
    explicit RkllmEngine(RkllmConfig config);
    ~RkllmEngine() override;
    RkllmEngine(const RkllmEngine&) = delete;
    RkllmEngine& operator=(const RkllmEngine&) = delete;
    std::string infer(std::string_view original) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
