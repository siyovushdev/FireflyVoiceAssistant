#include "firefly/RkllmEngine.h"
#include "rkllm.h"
#include "firefly/InputPolicy.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <thread>
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
// No detached work: destruction always waits for the abort watchdog to finish.
// A vendor hang may still delay rkllm_run/abort; this is not a hard realtime bound.
class RequestDeadline {
public:
    RequestDeadline(LLMHandle handle, std::chrono::milliseconds timeout)
        : worker_([this, handle, timeout] {
            std::unique_lock<std::mutex> lock(mutex_);
            if (cv_.wait_for(lock, timeout, [this] { return done_; })) return;
            expired_ = true;
            while (!done_) {
                const int running = rkllm_is_running(handle);
                if (running == 1) {
                    abortResult_ = rkllm_abort(handle);
                    return;
                }
                if (running != 0) { abortResult_ = running; return; }
                cv_.wait_for(lock, std::chrono::milliseconds(1), [this] { return done_; });
            }
        }) {}
    ~RequestDeadline() { finish(); }
    void finish() {
        { std::lock_guard<std::mutex> lock(mutex_); done_ = true; }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    bool expired() const { return expired_; }
    int abortResult() const { return abortResult_; }
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;
    bool expired_ = false;
    int abortResult_ = 0;
    std::thread worker_;
};

}
struct RkllmEngine::Impl {
    explicit Impl(RkllmConfig value) : config(std::move(value)) {}
    ~Impl() {
        if (handle) {
            const int result = rkllm_destroy(handle);
            if (result != 0) std::cerr << "rkllm_destroy failed: " << result << '\n';
            else if (config.verbose) std::cerr << "RKLLM destroyed\n";
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
    bool ready = false;
    int bootstrapPrefill = 0;

    static int callback(RKLLMResult* result, void* userdata, LLMCallState state) noexcept {
        auto* self = static_cast<Impl*>(userdata);
        if (!self) return 0;
        // C callbacks must never unwind into RKLLM. The synchronous caller checks errors.
        try {
            std::lock_guard<std::mutex> lock(self->outputMutex);
            if (self->finished) { self->callbackFailed = true; return 0; }
            switch (state) {
                case RKLLM_RUN_NORMAL:
                    if (!result || !result->text) { self->callbackFailed = true; break; }
                    // Bound structured output and faulty callbacks without repair.
                    for (const char* c = result->text; *c; ++c) {
                        if (self->output.size() >= 4096) { self->callbackFailed = true; break; }
                        self->output.push_back(*c);
                    }
                    break;
                case RKLLM_RUN_WAITING: break; // Documented incomplete UTF-8 state.
                case RKLLM_RUN_FINISH:
                    self->finished = true;
                    if (result) self->perf = result->perf;
                    break;
                case RKLLM_RUN_ERROR: self->callbackFailed = true; break;
                default: self->callbackFailed = true; break;
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
        infer.max_new_tokens = RkllmResponseTokens;
        resetOutput();
        const auto start = Clock::now();
        RequestDeadline deadline(handle, config.requestTimeout);
        const int status = rkllm_run(handle, &input, &infer, this);
        deadline.finish();
        if (deadline.expired())
            throw std::runtime_error("RKLLM deadline exceeded; abort status=" + std::to_string(deadline.abortResult()));
        requireSuccess(status, "rkllm_run");
        std::lock_guard<std::mutex> lock(outputMutex);
        if (callbackFailed || !finished) throw std::runtime_error("RKLLM callback failed or did not finish");
        // The public API has no internal-tokenizer preflight. Admit actual runtime
        // counts before READY / before returning any response; never estimate tokens.
        int cacheSize = -1;
        requireSuccess(rkllm_get_kv_cache_size(handle, &cacheSize), "read KV size");
        const int ceiling = ready ? config.requestTokenReserve :
                            RkllmContextTokens - RkllmResponseTokens - config.requestTokenReserve;
        if (perf.prefill_tokens <= 0 || perf.prefill_tokens > ceiling ||
            perf.generate_tokens < 0 || perf.generate_tokens >= RkllmResponseTokens - 1 ||
            cacheSize <= 0 || cacheSize > RkllmContextTokens ||
            (ready && bootstrapPrefill + perf.prefill_tokens + RkllmResponseTokens > RkllmContextTokens))
            throw std::runtime_error("RKLLM context/output budget rejected: prefill=" + std::to_string(perf.prefill_tokens) +
                                     " generated=" + std::to_string(perf.generate_tokens) + " kv=" + std::to_string(cacheSize));
        if (!ready) {
            bootstrapPrefill = perf.prefill_tokens;
            std::cerr << "LLM_CONTEXT bootstrap_prefill=" << bootstrapPrefill << " bootstrap_kv=" << cacheSize
                      << " ceiling=" << ceiling << " request_reserve=" << config.requestTokenReserve
                      << " response_budget=" << RkllmResponseTokens << " context=" << RkllmContextTokens << '\n';
        }
        if (config.verbose)
            std::cerr << label << " wall_ms=" << elapsed(start)
                      << " prefill_ms=" << perf.prefill_time_ms << " prefill_tokens=" << perf.prefill_tokens
                      << " generate_ms=" << perf.generate_time_ms << " generate_tokens=" << perf.generate_tokens << '\n';
        return output;
    }
    void resetOutput() {
        std::lock_guard<std::mutex> lock(outputMutex);
        output.clear();
        perf = {};
        callbackFailed = false;
        finished = false;
    }
    void initialize() {
        if (config.requestTokenReserve < 128 || config.requestTokenReserve > RkllmRequestReserve)
            throw std::invalid_argument("Invalid request token reservation");
        if (config.requestTimeout.count() <= 0) throw std::invalid_argument("Timeout must be positive");
        if (config.modelPath.empty() || config.systemPrompt.empty() ||
            config.modelPath.find('\0') != std::string::npos || config.systemPrompt.find('\0') != std::string::npos)
            throw std::invalid_argument("Model path and system prompt must be nonempty and contain no NUL bytes");
        if (config.systemPrompt.size() > 16384)
            throw std::invalid_argument("LLM system prompt exceeds byte resource limit");
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
        param.max_new_tokens = RkllmResponseTokens;
        param.max_context_len = RkllmContextTokens;
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
        ready = true;
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
    impl_->resetOutput(); // Includes failures before rkllm_run, e.g. KV clear failure.
    if (!validInput(original)) throw std::invalid_argument("Invalid/oversized input");
    if (!impl_->ready) throw std::runtime_error("RKLLM engine not ready; restart required");
    try {
        requireSuccess(rkllm_clear_kv_cache(impl_->handle, 1, nullptr, nullptr), "request clear KV");
        return impl_->run(original, "inference");
    } catch (...) {
        impl_->ready = false; // Do not reuse uncertain KV/runtime state after a fault.
        impl_->resetOutput();
        throw;
    }
}
void RkllmEngine::setSystemPrompt(std::string prompt) {
    std::lock_guard<std::mutex> lock(impl_->requestMutex);
    if (prompt.empty() || prompt.size() > 16384 || prompt.find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid system context");
    if (!impl_->ready) throw std::runtime_error("RKLLM engine not ready; restart required");
    if (prompt == impl_->config.systemPrompt) return;
    const auto old = impl_->config.systemPrompt;
    const auto install = [&](const std::string& value) {
        impl_->ready = false;
        impl_->resetOutput();
        requireSuccess(rkllm_clear_kv_cache(impl_->handle, 0, nullptr, nullptr), "context full KV clear");
        impl_->config.systemPrompt = value;
        impl_->systemChat = "<|im_start|>system\n" + value + "<|im_end|>\n";
        requireSuccess(rkllm_set_chat_template(impl_->handle, impl_->systemChat.c_str(),
            "<|im_start|>user\n", "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n"), "context chat template");
        (void)impl_->run("", "context bootstrap");
        requireSuccess(rkllm_clear_kv_cache(impl_->handle, 1, nullptr, nullptr), "context bootstrap clear");
        impl_->ready = true;
    };
    try { install(prompt); }
    catch (...) {
        const auto failure = std::current_exception();
        try { install(old); }
        catch (...) {
            impl_->ready = false;
            throw std::runtime_error("RKLLM context restoration failed; restart required");
        }
        std::rethrow_exception(failure);
    }
}

bool RkllmEngine::ready() const {
    std::lock_guard<std::mutex> lock(impl_->requestMutex);
    return impl_->ready;
}

}
