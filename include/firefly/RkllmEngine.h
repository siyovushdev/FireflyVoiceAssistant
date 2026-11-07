#pragma once
#include "firefly/LlmEngine.h"
#include <functional>
#include <chrono>
#include <memory>
#include <stdexcept>
namespace firefly {
// A bounded Admin document does not fit the existing context. The caller may
// shorten it and retry after the previous context was restored successfully.
class AdminContextBudgetError final : public std::runtime_error {
public:
    explicit AdminContextBudgetError(const std::string& detail)
        : std::runtime_error("admin_context_budget: " + detail) {}
};
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
    // Admission reservation, not sampling/context-size tuning. Admin uses 2048
    // for ordinary requests, or 128 for a pre-admitted whole document plus its
    // fixed short instruction. Ordinary CLI keeps M8 defaults.
    int requestTokenReserve = RkllmRequestReserve;
    std::chrono::milliseconds requestTimeout{30000};
    // Whole-document Admin plans may contain several hundred output tokens.
    // This finite watchdog applies only to the 768-output/128-request profile;
    // ordinary Admin and Runtime retain their existing deadlines. Not a user
    // setting. A smaller value also permits bounded watchdog regression tests.
    std::chrono::milliseconds adminDocumentTimeout{240000};
    // Explicit profile; defaults preserve the command model contract.
    int contextLimit = RkllmContextTokens;
    bool adminProfile = false;
    std::function<void(std::string_view)> progress;
    // Synchronous resource admission/diagnostics, invoked outside vendor callbacks.
    std::function<void(std::string_view)> resourcePhase;
};
struct InferenceMetrics {
    int contextTokens = 0, inputTokens = 0, outputTokens = 0, outputBudget = 128;
    double wallMs = 0;
    std::string finish = "not_run";
    double prefillMs = 0, generationMs = 0;
};
// Owns one synchronous runtime. Requests are serialized; do not destroy while in use.
class RkllmEngine final : public LlmEngine {
public:
    explicit RkllmEngine(RkllmConfig config);
    ~RkllmEngine() override;
    RkllmEngine(const RkllmEngine&) = delete;
    RkllmEngine& operator=(const RkllmEngine&) = delete;
    std::string infer(std::string_view original) override;
    std::string inferAdmin(std::string_view step);
    // Fails closed if vendor teardown cannot be confirmed. Never load another
    // model after this throws; process restart is required.
    void close();
    // Trusted server envelope only; original input retains the 192-byte policy.
    std::string inferCommand(std::string_view original, std::string_view contract);
    // Serialized full KV/context refresh. Restores the old context on failure.
    // A failed restoration makes inference unavailable until engine restart.
    // An explicit request reservation is Admin-only. Zero retains the current
    // reservation. Document contexts reserve 128 for a fixed short instruction;
    // ordinary Admin requests restore 2048. Runtime admission is unchanged.
    void setSystemPrompt(std::string prompt, int responseTokens = RkllmResponseTokens,
                         int requestTokenReserve = 0);
    bool ready() const;
    int contextTokens() const;
    InferenceMetrics metrics() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
