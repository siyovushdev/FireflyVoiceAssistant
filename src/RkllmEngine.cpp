#include "firefly/RkllmEngine.h"
#include "rkllm.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace firefly {
namespace {
using Clock = std::chrono::steady_clock;
long long elapsed(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}
void requireSuccess(int result, const char* operation) {
    if (result != 0) throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
}
}
struct RkllmEngine::Impl {
    explicit Impl(RkllmConfig value) : config(std::move(value)) {}
    ~Impl() {
        if (handle) {
            const int result = rkllm_destroy(handle);
            if (result != 0) std::cerr << "rkllm_destroy failed: " << result << '\n';
        }
    }
    RkllmConfig config;
    LLMHandle handle = nullptr;
    std::string systemChat;
    RKLLMCallback callbacks{};
    std::mutex requestMutex;
    std::mutex outputMutex;
    std::string output;
    RKLLMPerfStat perf{};
    std::atomic<bool> callbackFailed{false};
    bool finished = false;

    static int callback(RKLLMResult* result, void* userdata, LLMCallState state) noexcept {
        auto* self = static_cast<Impl*>(userdata);
        if (!self) return 0;
        // C callbacks must never unwind into RKLLM. The synchronous caller checks errors.
        try {
            std::lock_guard<std::mutex> lock(self->outputMutex);
            if (state == RKLLM_RUN_ERROR) self->callbackFailed = true;
            if (state == RKLLM_RUN_NORMAL && result && result->text)
                self->output += result->text;
            if (state == RKLLM_RUN_FINISH) {
                self->finished = true;
                if (result) self->perf = result->perf;
            }
        } catch (...) { self->callbackFailed = true; }
        return 0;
    }
    void progress(std::string_view stage) {
        if (config.progress) config.progress(stage);
    }
    std::string run(std::string_view original, const char* label) {
        if (original.find('\0') != std::string_view::npos)
            throw std::invalid_argument("Input contains a NUL byte");
        std::string prompt(original);
        RKLLMInput input{};
        input.input_type = RKLLM_INPUT_PROMPT;
        input.role = "user";
        input.enable_thinking = false;
        input.prompt_input = prompt.data();
        RKLLMInferParam infer{};
        infer.mode = RKLLM_INFER_GENERATE;
        infer.keep_history = 0;
        infer.max_new_tokens = 16;
        {
            std::lock_guard<std::mutex> lock(outputMutex);
            output.clear();
            perf = {};
            callbackFailed = false;
            finished = false;
        }
        const auto start = Clock::now();
        requireSuccess(rkllm_run(handle, &input, &infer, this), "rkllm_run");
        std::lock_guard<std::mutex> lock(outputMutex);
        if (callbackFailed || !finished) throw std::runtime_error("RKLLM callback failed or did not finish");
        if (config.verbose)
            std::cerr << label << " wall_ms=" << elapsed(start)
                      << " prefill_ms=" << perf.prefill_time_ms << " prefill_tokens=" << perf.prefill_tokens
                      << " generate_ms=" << perf.generate_time_ms << " generate_tokens=" << perf.generate_tokens << '\n';
        return output;
    }
    void initialize() {
        if (config.modelPath.empty() || config.systemPrompt.empty() ||
            config.modelPath.find('\0') != std::string::npos || config.systemPrompt.find('\0') != std::string::npos)
            throw std::invalid_argument("Model path and system prompt must be nonempty and contain no NUL bytes");
        const auto startup = Clock::now();
        progress("Loading RKLLM...");
        auto param = rkllm_createDefaultParam();
        param.model_path = config.modelPath.c_str();
        param.top_k = 1;
        param.top_p = 1.0f;
        param.temperature = 0.1f;
        param.repeat_penalty = 1.0f;
        param.frequency_penalty = 0.0f;
        param.presence_penalty = 0.0f;
        param.max_new_tokens = 16;
        param.max_context_len = 1024;
        param.skip_special_token = true;
        param.extend_param.base_domain_id = 0;
        param.extend_param.embed_flash = 1;
        callbacks.result_callback = callback;
        callbacks.result_userdata = this;
        progress("Loading model...");
        const auto initStart = Clock::now();
        requireSuccess(rkllm_init(&handle, &param, &callbacks), "rkllm_init");
        if (config.verbose) std::cerr << "init_ms=" << elapsed(initStart) << '\n';
        systemChat = "<|im_start|>system\n" + config.systemPrompt + "<|im_end|>\n";
        requireSuccess(rkllm_set_chat_template(handle, systemChat.c_str(),
            "<|im_start|>user\n", "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n"),
            "rkllm_set_chat_template");
        progress("Building system KV...");
        (void)run("", "bootstrap");
        requireSuccess(rkllm_clear_kv_cache(handle, 1, nullptr, nullptr), "bootstrap clear KV");
        if (config.verbose) std::cerr << "startup_ms=" << elapsed(startup) << '\n';
    }
};
RkllmEngine::RkllmEngine(RkllmConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {
    // Separate initialization ensures Impl destroys any acquired handle on failure.
    impl_->initialize();
}
RkllmEngine::~RkllmEngine() = default;
std::string RkllmEngine::infer(std::string_view original) {
    std::lock_guard<std::mutex> lock(impl_->requestMutex);
    requireSuccess(rkllm_clear_kv_cache(impl_->handle, 1, nullptr, nullptr), "request clear KV");
    return impl_->run(original, "inference");
}
}
