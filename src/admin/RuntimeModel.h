#pragma once
#include "firefly/admin/RuntimeGeneration.h"
#include "firefly/RkllmEngine.h"
#include <sstream>
namespace firefly::admin {
class RuntimeModel final : public firefly::admin::ContextModel {
public:
    explicit RuntimeModel(std::string path) : path_(std::move(path)) {}
    llm::CommandProtocol commandProtocol() const override { return llm::CommandProtocol::StableCompact; }
    void useContext(const std::string& prompt) override {
        if (path_.empty()) throw std::runtime_error("Model not configured");
        if (!engine_ || !engine_->ready()) {
            // Reinitialization is only recovery after a fatal runtime failure.
            engine_.reset();
            firefly::RkllmConfig config;
            config.modelPath = path_; config.systemPrompt = prompt;
            config.verbose = true; config.requestTokenReserve = 128;
            engine_ = std::make_unique<firefly::RkllmEngine>(std::move(config));
        } else engine_->setSystemPrompt(prompt);
    }
    int contextTokens() const override { return engine_ ? engine_->contextTokens() : -1; }
    void useAdminContext(const std::string& prompt, int tokens) override {
        if (!engine_ || !engine_->ready()) useContext(prompt);
        engine_->setSystemPrompt(prompt, tokens);
    }
    std::string inferenceMetrics() const override {
        if (!engine_) return "{}";
        const auto m = engine_->metrics();
        std::ostringstream out;
        out << "{\"context_tokens\":" << m.contextTokens << ",\"input_tokens\":" << m.inputTokens
            << ",\"output_tokens\":" << m.outputTokens << ",\"output_budget\":" << m.outputBudget
            << ",\"prefill_ms\":" << m.prefillMs << ",\"generation_ms\":" << m.generationMs
            << ",\"wall_ms\":" << m.wallMs << ",\"finish\":\"" << m.finish << "\"}";
        return out.str();
    }
    bool ready() const override { return engine_ && engine_->ready(); }
    std::string inferCommand(std::string_view text, std::string_view contract) override {
        if (!engine_) throw std::runtime_error("Model unavailable");
        return engine_->inferCommand(text, contract);
    }
    std::string infer(std::string_view text) override {
        if (!engine_) throw std::runtime_error("Model unavailable");
        return engine_->infer(text);
    }
private:
    std::string path_;
    std::unique_ptr<firefly::RkllmEngine> engine_;
};
}
