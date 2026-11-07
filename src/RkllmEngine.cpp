#include "firefly/RkllmEngine.h"
#include "rkllm.h"
#include "firefly/InputPolicy.h"
#include "firefly/admin/AdminRequestPolicy.h"
#include <atomic>
#include <algorithm>
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
// One physical NPU model owner in this process, including partially initialized
// handles. A failed destroy retains the lease: recovery requires process restart.
std::atomic<bool> runtimeOwned{false};
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
    explicit Impl(RkllmConfig value) : config(std::move(value)) {
        if (config.adminProfile) responseTokens = 768;
    }
    ~Impl() {
        if (destroyFailed) return; // Ownership is uncertain; never retry this handle.
        if (handle) {
            const int result = rkllm_destroy(handle);
            if (result != 0) { std::cerr << "rkllm_destroy failed: " << result << '\n'; return; }
            if (config.verbose) std::cerr << "RKLLM destroyed\n";
        }
        if (ownsRuntime) runtimeOwned = false;
    }
    RkllmConfig config;
    LLMHandle handle = nullptr;
    bool ownsRuntime = false;
    bool destroyFailed = false;
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
    int responseTokens = RkllmResponseTokens;
    InferenceMetrics lastMetrics;

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
    std::string run(std::string_view original, const char* label, int requestReserve = 0) {
        if (original.find('\0') != std::string_view::npos)
            throw std::invalid_argument("Input contains a NUL byte");
        const bool bootstrap = !ready;
        if (config.resourcePhase) config.resourcePhase(bootstrap ? "before_bootstrap" : "before_inference");
        std::string prompt(original);
        RKLLMInput input{};
        input.input_type = RKLLM_INPUT_PROMPT;
        input.role = "user";
        input.enable_thinking = false;
        input.prompt_input = prompt.data();
        RKLLMInferParam infer{};
        infer.mode = RKLLM_INFER_GENERATE;
        infer.keep_history = 0;
        // Admin bootstrap only establishes system KV; its answer is discarded.
        // One token avoids spending a full draft budget on the empty bootstrap.
        infer.max_new_tokens = !ready && (config.adminProfile || responseTokens > RkllmResponseTokens) ? 1 : responseTokens;
        resetOutput();
        const auto start = Clock::now();
        const bool documentProfile = config.adminProfile && responseTokens == 768 && config.requestTokenReserve == 128;
        const auto timeout = documentProfile ? config.adminDocumentTimeout :
            responseTokens > RkllmResponseTokens ? std::max(config.requestTimeout,std::chrono::milliseconds(60000)) : config.requestTimeout;
        RequestDeadline deadline(handle, timeout);
        const int status = rkllm_run(handle, &input, &infer, this);
        deadline.finish();
        if (deadline.expired())
            throw std::runtime_error("RKLLM deadline exceeded; abort status=" + std::to_string(deadline.abortResult()));
        requireSuccess(status, "rkllm_run");
        if (config.resourcePhase) config.resourcePhase(bootstrap ? "after_bootstrap" : "after_inference");
        std::lock_guard<std::mutex> lock(outputMutex);
        if (callbackFailed || !finished) throw std::runtime_error("RKLLM callback failed or did not finish");
        // The public API has no internal-tokenizer preflight. Admit actual runtime
        // counts before READY / before returning any response; never estimate tokens.
        int cacheSize = -1;
        requireSuccess(rkllm_get_kv_cache_size(handle, &cacheSize), "read KV size");
        const int ceiling = ready ? (requestReserve ? requestReserve : config.requestTokenReserve) :
                            config.contextLimit - responseTokens - config.requestTokenReserve;
        // Only a measured, otherwise valid Admin bootstrap overflow is an
        // editable document rejection. Bad callbacks/KV/perf retain the fatal
        // failure classification. setSystemPrompt restores the old context
        // before this error can be returned to its caller.
        if (bootstrap && config.adminProfile && perf.prefill_tokens > ceiling &&
            perf.generate_tokens >= 0 && perf.generate_tokens < responseTokens - 1 &&
            cacheSize > 0 && cacheSize <= config.contextLimit)
            throw AdminContextBudgetError("document and instructions exceed the 4096-token context; shorten the request");
        if (perf.prefill_tokens <= 0 || perf.prefill_tokens > ceiling ||
            perf.generate_tokens < 0 || perf.generate_tokens >= responseTokens - 1 ||
            cacheSize <= 0 || cacheSize > config.contextLimit ||
            (ready && bootstrapPrefill + perf.prefill_tokens + responseTokens > config.contextLimit))
            throw std::runtime_error("RKLLM context/output budget rejected: prefill=" + std::to_string(perf.prefill_tokens) +
                                     " generated=" + std::to_string(perf.generate_tokens) + " kv=" + std::to_string(cacheSize));
        if (!ready) {
            bootstrapPrefill = perf.prefill_tokens;
            std::cerr << "LLM_CONTEXT bootstrap_prefill=" << bootstrapPrefill << " bootstrap_kv=" << cacheSize
                      << " ceiling=" << ceiling << " request_reserve=" << config.requestTokenReserve
                      << " response_budget=" << responseTokens << " context=" << config.contextLimit << '\n';
        }
        if (config.verbose)
            std::cerr << label << " wall_ms=" << elapsed(start)
                      << " prefill_ms=" << perf.prefill_time_ms << " prefill_tokens=" << perf.prefill_tokens
                      << " generate_ms=" << perf.generate_time_ms << " generate_tokens=" << perf.generate_tokens << '\n';
        if (ready) lastMetrics = {bootstrapPrefill, perf.prefill_tokens, perf.generate_tokens, responseTokens, static_cast<double>(elapsed(start)), "vendor_finish_below_limit", perf.prefill_time_ms, perf.generate_time_ms};
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
        if ((config.adminProfile && config.contextLimit != 4096) || (!config.adminProfile && config.contextLimit != 1024))
            throw std::invalid_argument("Invalid model profile context");
        if (config.requestTokenReserve < 128 || config.requestTokenReserve > (config.adminProfile ? 2048 : RkllmRequestReserve))
            throw std::invalid_argument("Invalid request token reservation");
        if (config.requestTimeout.count() <= 0) throw std::invalid_argument("Timeout must be positive");
        if (config.adminDocumentTimeout.count() <= 0 || config.adminDocumentTimeout > std::chrono::milliseconds(240000))
            throw std::invalid_argument("Admin document timeout must be positive and at most 240000 ms");
        if (config.modelPath.empty() || config.systemPrompt.empty() ||
            config.modelPath.find('\0') != std::string::npos || config.systemPrompt.find('\0') != std::string::npos)
            throw std::invalid_argument("Model path and system prompt must be nonempty and contain no NUL bytes");
        if (config.systemPrompt.size() > (config.adminProfile ? 65536u : 16384u))
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
        param.max_context_len = config.contextLimit;
        param.skip_special_token = true;
        param.extend_param.base_domain_id = 0;
        param.extend_param.embed_flash = 1;
        callbacks.result_callback = callback;
        callbacks.result_userdata = this;
        progress("Loading model...");
        if (config.resourcePhase) config.resourcePhase("before_init");
        bool expected = false;
        if (!runtimeOwned.compare_exchange_strong(expected, true)) throw std::runtime_error("rkllm_owner_busy");
        ownsRuntime = true;
        const auto initStart = Clock::now();
        requireSuccess(rkllm_init(&handle, &param, &callbacks), "rkllm_init");
        if (config.resourcePhase) config.resourcePhase("after_init");
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
void RkllmEngine::close() {
    std::lock_guard<std::mutex> lock(impl_->requestMutex);
    impl_->ready = false;
    if (impl_->destroyFailed) throw std::runtime_error("rkllm_destroy; restart_required");
    if (!impl_->handle) return;
    const int status = rkllm_destroy(impl_->handle);
    if (status != 0) impl_->destroyFailed = true;
    requireSuccess(status, "rkllm_destroy; restart_required");
    impl_->handle = nullptr;
    impl_->ownsRuntime = false;
    runtimeOwned = false;
}
std::string RkllmEngine::inferAdmin(std::string_view step) {
    admin::validateMessage(step);
    std::lock_guard<std::mutex> lock(impl_->requestMutex);
    if (!impl_->config.adminProfile || !impl_->ready)
        throw std::runtime_error("Admin model unavailable");
    // Conservative byte upper bound, not a tokenizer. Actual token checks in
    // run() remain mandatory. Whole-document Setup admits the user's original
    // text in system DATA and calls here with only its fixed short instruction.
    if (step.size() > static_cast<std::size_t>(impl_->config.requestTokenReserve))
        throw std::invalid_argument("admin_step_budget: split the structured task before inference");
    impl_->resetOutput();
    try {
        requireSuccess(rkllm_clear_kv_cache(impl_->handle, 1, nullptr, nullptr), "admin clear KV");
        return impl_->run(step, "admin inference");
    } catch (...) { impl_->ready = false; impl_->resetOutput(); throw; }
}
std::string RkllmEngine::inferCommand(std::string_view original, std::string_view contract) {
    // The contract is constructed from the admitted immutable catalog, never from
    // model output. Escape user data, including chat delimiters, inside JSON.
    if (!validInput(original) || contract.empty() || contract.size() > 1024 ||
        contract.find('\0') != std::string_view::npos)
        throw std::invalid_argument("Invalid command envelope");
    std::string quoted = "\"";
    for (unsigned char c : original) {
        if (c == '<') quoted += "\\u003c";
        else if (c == '"' || c == '\\') { quoted += '\\'; quoted += c; }
        else if (c < 32) { const char* hex = "0123456789abcdef"; quoted += "\\u00"; quoted += hex[c >> 4]; quoted += hex[c & 15]; }
        else quoted += c;
    }
    quoted += '"';
    std::string schema;
    for (char c : contract) { if (c == '<') schema += "\\u003c"; else schema += c; }
    const std::string envelope = "{\"schema\":" + schema + ",\"text\":" + quoted + "}";
    std::lock_guard<std::mutex> lock(impl_->requestMutex);
    impl_->resetOutput();
    if (!impl_->ready) throw std::runtime_error("RKLLM engine not ready; restart required");
    try {
        requireSuccess(rkllm_clear_kv_cache(impl_->handle, 1, nullptr, nullptr), "command clear KV");
        return impl_->run(envelope, "command inference", RkllmRequestReserve);
    } catch (...) {
        impl_->ready = false;
        impl_->resetOutput();
        throw;
    }
}
void RkllmEngine::setSystemPrompt(std::string prompt, int responseTokens, int requestTokenReserve) {
    std::lock_guard<std::mutex> lock(impl_->requestMutex);
    if (requestTokenReserve != 0 && (!impl_->config.adminProfile || requestTokenReserve < 128 || requestTokenReserve > 2048))
        throw std::invalid_argument("Invalid Admin request reservation");
    if (prompt.size() > (impl_->config.adminProfile ? 65536u : 16384u)) {
        if (impl_->config.adminProfile) throw AdminContextBudgetError("document exceeds the bounded context byte limit; shorten the request");
        throw std::invalid_argument("Invalid system context");
    }
    if ((responseTokens != 128 && responseTokens != 256 && responseTokens != 384 && !(impl_->config.adminProfile && responseTokens == 768)) || prompt.empty() || prompt.find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid system context");
    if (!impl_->ready) throw std::runtime_error("RKLLM engine not ready; restart required");
    const int reservation = requestTokenReserve ? requestTokenReserve : impl_->config.requestTokenReserve;
    if (prompt == impl_->config.systemPrompt && responseTokens == impl_->responseTokens && reservation == impl_->config.requestTokenReserve) return;
    const auto old = impl_->config.systemPrompt;
    const auto oldBudget = impl_->responseTokens;
    const auto oldReservation = impl_->config.requestTokenReserve;
    const auto install = [&](const std::string& value, int budget, int reserve) {
        impl_->responseTokens = budget;
        impl_->config.requestTokenReserve = reserve;
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
    try { install(prompt, responseTokens, reservation); }
    catch (...) {
        const auto failure = std::current_exception();
        try { install(old, oldBudget, oldReservation); }
        catch (...) {
            impl_->ready = false;
            throw std::runtime_error("RKLLM context restoration failed; restart required");
        }
        std::rethrow_exception(failure);
    }
}

int RkllmEngine::contextTokens() const {
    std::lock_guard<std::mutex> lock(impl_->requestMutex);
    return impl_->ready ? impl_->bootstrapPrefill : -1;
}

InferenceMetrics RkllmEngine::metrics() const {
    std::lock_guard<std::mutex> lock(impl_->requestMutex);
    return impl_->lastMetrics;
}

bool RkllmEngine::ready() const {
    std::lock_guard<std::mutex> lock(impl_->requestMutex);
    return impl_->ready;
}

}
