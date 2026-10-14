#pragma once
#include "firefly/admin/RuntimeGeneration.h"
#include "firefly/RkllmEngine.h"
#include "firefly/SetupResources.h"
#include <cstdint>
#include <sstream>
#include <iostream>
#include <fstream>
#include "../config/Platform.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
namespace firefly::admin {
class RuntimeModel final : public firefly::admin::ContextModel {
public:
    static constexpr std::int64_t AdminModelBytes = 4848176220LL;
    static constexpr char AdminModelSha256[] = "58e8e74d8f129ea6f68b8e2101aae65d512668f46bb847a89e88a6a6d6094e5b";
    // Runtime model path is the 1024 profile. Admin 4096 is a separate explicit path;
    // an empty admin path fails closed and never falls back to a developer tree.
    explicit RuntimeModel(std::string path, std::string adminPath = {},
                          std::shared_ptr<SetupResources> resources = std::make_shared<SetupResources>(),
                          std::function<void(const std::string&)> verify = verifyAdminModel)
        : path_(std::move(path)), adminPath_(std::move(adminPath)), resources_(std::move(resources)), verify_(std::move(verify)) {
        if (!resources_ || !verify_) throw std::invalid_argument("Missing resource policy");
    }
    static void checkAdminModelFile(const std::string& path) {
        if (path.empty()) throw std::runtime_error("admin_model_not_configured");
        config::detail::FileDescriptor file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        struct stat info{};
        if (file.get() < 0 || ::fstat(file.get(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size != AdminModelBytes)
            throw std::runtime_error("admin_model_unavailable_or_wrong_size");
    }
    llm::CommandProtocol commandProtocol() const override { return llm::CommandProtocol::StableCompact; }
    void useContext(const std::string& prompt) override {
        auto lease = resources_->acquire();
        useContextLocked(prompt);
    }
private:
    void useContextLocked(const std::string& prompt) {
        if (admin_) throw std::runtime_error("setup_in_progress");
        if (poisoned_) throw std::runtime_error("restart_required");
        if (path_.empty()) throw std::runtime_error("Model not configured");
        if (!engine_ || !engine_->ready()) {
            // Reinitialization is only recovery after a fatal runtime failure.
            release();
            firefly::RkllmConfig config;
            config.modelPath = path_; config.systemPrompt = prompt;
            config.verbose = true; config.requestTokenReserve = 128;
            configureResources(config);
            baselineDma_ = resources_->checkpoint("runtime_before_load").dmaBytes;
            try { engine_ = std::make_unique<firefly::RkllmEngine>(std::move(config)); }
            catch (...) {
                resources_->note("runtime_load_failed");
                try { resources_->awaitReleased(baselineDma_); }
                catch (...) { poisoned_ = true; throw; }
                throw;
            }
        } else engine_->setSystemPrompt(prompt);
    }
public:
    int contextTokens() const override { return engine_ ? engine_->contextTokens() : -1; }
    void useAdminContext(const std::string& prompt, int tokens) override {
        auto lease = resources_->acquire();
        if (!admin_ || !engine_ || !engine_->ready()) throw std::runtime_error("setup_required");
        resources_->checkpoint("admin_before_context", SetupResources::GiBKiB);
        engine_->setSystemPrompt(prompt, tokens);
    }
    bool setupRequired() const override { return true; }
    void enterSetup() override {
        auto lease = resources_->acquire();
        if (poisoned_) throw std::runtime_error("restart_required");
        if (admin_) throw std::runtime_error("setup_busy");
        release();
        resources_->awaitAdmission(6 * SetupResources::GiBKiB);
        resources_->checkpoint("admin_before_verify", 6 * SetupResources::GiBKiB);
        verify_(adminPath_);
        baselineDma_ = resources_->checkpoint("admin_before_load", 6 * SetupResources::GiBKiB).dmaBytes;
        RkllmConfig profile;
        profile.modelPath = adminPath_; profile.adminProfile = true;
        profile.contextLimit = 4096; profile.requestTokenReserve = 2048;
        profile.requestTimeout = std::chrono::milliseconds(60000);
        profile.systemPrompt = "Помощник настройки. Только JSON. Ничего не выполняй.";
        profile.verbose = true;
        configureResources(profile);
        try { engine_ = std::make_unique<RkllmEngine>(profile); }
        catch (...) {
            resources_->note("admin_load_failed");
            // Constructor RAII has destroyed any partial handle. Confirm DMA
            // release before RuntimeGenerations may restore the command model.
            try { resources_->awaitReleased(baselineDma_); }
            catch (...) { poisoned_ = true; throw; }
            throw;
        }
        admin_ = true;
    }
    static void verifyAdminModel(const std::string& path) {
        checkAdminModelFile(path);
        config::detail::FileDescriptor file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        struct stat info{};
        if (file.get() < 0 || ::fstat(file.get(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size != AdminModelBytes)
            throw std::runtime_error("admin_model_unavailable_or_wrong_size");
        void* bytes = ::mmap(nullptr,info.st_size,PROT_READ,MAP_PRIVATE,file.get(),0);
        if (bytes == MAP_FAILED) throw std::runtime_error("admin_model_verification_failed");
        std::string hash;
        try { hash = config::detail::sha256(std::string_view(static_cast<const char*>(bytes),info.st_size)); }
        catch (...) { ::munmap(bytes,info.st_size); throw; }
        ::munmap(bytes,info.st_size);
        if (hash != AdminModelSha256)
            throw std::runtime_error("admin_model_sha256_mismatch");
    }
    void leaveSetup(const std::string& prompt) override {
        auto lease = resources_->acquire();
        resources_->note("before_restore");
        try { release(); admin_ = false; useContextLocked(prompt); }
        catch (...) { resources_->note("restore_failed"); throw; }
        resources_->note("after_restore");
    }
    std::string inferAdmin(std::string_view text) override {
        auto lease = resources_->acquire();
        if (!admin_ || !engine_) throw std::runtime_error("setup_required");
        try { return engine_->inferAdmin(text); }
        catch (...) { resources_->note("admin_inference_failed"); throw; }
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
        auto lease = resources_->acquire();
        if (!engine_) throw std::runtime_error("Model unavailable");
        return engine_->inferCommand(text, contract);
    }
    std::string infer(std::string_view text) override {
        auto lease = resources_->acquire();
        if (!engine_) throw std::runtime_error("Model unavailable");
        return engine_->infer(text);
    }
private:
    void configureResources(RkllmConfig& profile) {
        profile.resourcePhase = [this, admin = profile.adminProfile](std::string_view phase) {
            if (!admin && phase.find("inference") != std::string_view::npos) return;
            resources_->checkpoint(std::string(admin ? "admin_" : "runtime_") + std::string(phase),
                (admin && phase == "before_init" ? 6 : 1) * SetupResources::GiBKiB);
        };
    }
    void release() {
        if (!engine_) return;
        resources_->note("before_destroy");
        try {
            engine_->close();
            engine_.reset();
            resources_->note("after_destroy");
            resources_->awaitReleased(baselineDma_);
        } catch (...) { poisoned_ = true; resources_->note("destroy_failed"); throw; }
        std::cerr << "MODEL_RELEASED profile=" << (admin_ ? "admin4096" : "runtime1024") << '\n';
    }
    bool admin_ = false, poisoned_ = false;
    std::string path_;
    std::string adminPath_;
    std::unique_ptr<firefly::RkllmEngine> engine_;
    std::shared_ptr<SetupResources> resources_;
    std::function<void(const std::string&)> verify_;
    std::uint64_t baselineDma_ = 0;
};
}
