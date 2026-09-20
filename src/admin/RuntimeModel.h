#pragma once
#include "firefly/admin/RuntimeGeneration.h"
#include "firefly/RkllmEngine.h"
namespace firefly::admin {
class RuntimeModel final : public firefly::admin::ContextModel {
public:
    explicit RuntimeModel(std::string path) : path_(std::move(path)) {}
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
    bool ready() const override { return engine_ && engine_->ready(); }
    std::string infer(std::string_view text) override {
        if (!engine_) throw std::runtime_error("Model unavailable");
        return engine_->infer(text);
    }
private:
    std::string path_;
    std::unique_ptr<firefly::RkllmEngine> engine_;
};
}
