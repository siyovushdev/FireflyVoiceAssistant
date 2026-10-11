#pragma once
#include "firefly/admin/RuntimeGeneration.h"
#include "firefly/RkllmEngine.h"
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
    explicit RuntimeModel(std::string path, std::string adminPath = {})
        : path_(std::move(path)), adminPath_(std::move(adminPath)) {}
    static void checkAdminModelFile(const std::string& path) {
        if (path.empty()) throw std::runtime_error("admin_model_not_configured");
        config::detail::FileDescriptor file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        struct stat info{};
        if (file.get() < 0 || ::fstat(file.get(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size != AdminModelBytes)
            throw std::runtime_error("admin_model_unavailable_or_wrong_size");
    }
    llm::CommandProtocol commandProtocol() const override { return llm::CommandProtocol::StableCompact; }
    void useContext(const std::string& prompt) override {
        if (admin_) throw std::runtime_error("setup_in_progress");
        if (poisoned_) throw std::runtime_error("restart_required");
        if (path_.empty()) throw std::runtime_error("Model not configured");
        if (!engine_ || !engine_->ready()) {
            // Reinitialization is only recovery after a fatal runtime failure.
            release();
            firefly::RkllmConfig config;
            config.modelPath = path_; config.systemPrompt = prompt;
            config.verbose = true; config.requestTokenReserve = 128;
            engine_ = std::make_unique<firefly::RkllmEngine>(std::move(config));
        } else engine_->setSystemPrompt(prompt);
    }
    int contextTokens() const override { return engine_ ? engine_->contextTokens() : -1; }
    void useAdminContext(const std::string& prompt, int tokens) override {
        if (!admin_ || !engine_ || !engine_->ready()) throw std::runtime_error("setup_required");
        engine_->setSystemPrompt(prompt, tokens);
    }
    bool setupRequired() const override { return true; }
    void enterSetup() override {
        if (poisoned_) throw std::runtime_error("restart_required");
        if (admin_) throw std::runtime_error("setup_busy");
        release();
        requireMemory(6*1024*1024);
        checkAdminModelFile(adminPath_);
        config::detail::FileDescriptor file(::open(adminPath_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
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
        RkllmConfig profile;
        profile.modelPath = adminPath_; profile.adminProfile = true;
        profile.contextLimit = 4096; profile.requestTokenReserve = 2048;
        profile.requestTimeout = std::chrono::milliseconds(60000);
        profile.systemPrompt = "Помощник настройки. Только JSON. Ничего не выполняй.";
        profile.verbose = true;
        engine_ = std::make_unique<RkllmEngine>(profile);
        admin_ = true;
        try { requireMemory(1024*1024); } catch (...) { release(); admin_=false; throw; }
    }
    void leaveSetup(const std::string& prompt) override {
        release(); admin_ = false; useContext(prompt);
    }
    std::string inferAdmin(std::string_view text) override {
        if (!admin_ || !engine_) throw std::runtime_error("setup_required");
        requireMemory(1024*1024);
        return engine_->inferAdmin(text);
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
    static void requireMemory(std::uint64_t minimumKiB) {
        std::ifstream file("/proc/meminfo");std::string line;
        while(std::getline(file,line))if(line.rfind("MemAvailable:",0)==0) {
            std::istringstream row(line);std::string key;std::uint64_t available=0;row>>key>>available;
            if(available>=minimumKiB)return;
            throw std::runtime_error("admin_memory_reserve_unavailable");
        }
        throw std::runtime_error("memory_status_unavailable");
    }
    void release() {
        if (!engine_) return;
        try { engine_->close(); } catch (...) { poisoned_ = true; throw; }
        engine_.reset();
        std::cerr << "MODEL_RELEASED profile=" << (admin_ ? "admin4096" : "runtime1024") << '\n';
    }
    bool admin_ = false, poisoned_ = false;
    std::string path_;
    std::string adminPath_;
    std::unique_ptr<firefly::RkllmEngine> engine_;
};
}
