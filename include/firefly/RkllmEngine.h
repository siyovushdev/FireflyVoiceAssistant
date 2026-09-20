#pragma once
#include "firefly/LlmEngine.h"
#include <functional>
#include <chrono>
#include <memory>
namespace firefly {
// Structured output needs more than the old 16-token compact response budget.
// Sampling/model/context size and KV lifecycle remain unchanged.
inline constexpr int RkllmContextTokens = 1024;
inline constexpr int RkllmResponseTokens = 128;
inline constexpr int RkllmRequestReserve = 256;
inline constexpr int RkllmBootstrapCeiling = RkllmContextTokens - RkllmResponseTokens - RkllmRequestReserve;
struct RkllmConfig {
    std::string modelPath;
    std::string systemPrompt;
    bool verbose = false;
    // Admission reservation, not sampling/context-size tuning. Admin allows
    // larger catalogs with 128 request tokens; ordinary CLI keeps M8 defaults.
    int requestTokenReserve = RkllmRequestReserve;
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
    // Serialized full KV/context refresh. Restores the old context on failure.
    // A failed restoration makes inference unavailable until engine restart.
    void setSystemPrompt(std::string prompt);
    bool ready() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
