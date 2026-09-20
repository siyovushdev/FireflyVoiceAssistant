#include "FakeRkllmRuntime.h"
#include "firefly/RkllmEngine.h"
#include <chrono>
#include <thread>
#include <string>
#include <cstdlib>
#include <iostream>
namespace firefly_test { Stub stub; }
using namespace firefly_test;
extern "C" {
RKLLMParam rkllm_createDefaultParam() { return {}; }
int rkllm_init(LLMHandle* handle, RKLLMParam* param, RKLLMCallback* callback) {
    ++stub.init;
    if (std::getenv("FIREFLY_TEST_INIT_FAIL")) return -9;
    stub.callbacks = *callback;
    stub.settings = param->top_k == 1 && param->top_p == 1 && param->temperature == 0.1f &&
        param->max_new_tokens == firefly::RkllmResponseTokens && param->max_context_len == 1024 && param->repeat_penalty == 1 &&
        param->frequency_penalty == 0 && param->presence_penalty == 0 && param->skip_special_token;
    *handle = &stub;
    return 0;
}
int rkllm_set_chat_template(LLMHandle, const char* system, const char* prefix, const char* postfix) {
    stub.system = system;
    stub.events.push_back("template");
    if ((!stub.rejectTemplate.empty() && stub.system.find(stub.rejectTemplate) != std::string::npos) || stub.failRestoration) return -7;
    stub.settings = stub.settings && std::string(system) == "<|im_start|>system\nfrozen<|im_end|>\n" &&
        std::string(prefix) == "<|im_start|>user\n" &&
        std::string(postfix) == "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n";
    return 0;
}
int rkllm_clear_kv_cache(LLMHandle, int keep, int* start, int* end) {
    ++stub.clear;
    stub.events.push_back("clear:" + std::to_string(keep));
    stub.independent = stub.independent && keep == 1 && !start && !end;
    return stub.fault == Fault::Clear ? -2 : 0;
}
int rkllm_get_kv_cache_size(LLMHandle, int* sizes) {
    *sizes = stub.fault == Fault::CacheSize ? 1025 : 600;
    return stub.fault == Fault::CacheRead ? -4 : 0;
}
int rkllm_destroy(LLMHandle) { ++stub.destroy; std::cerr << "stub destroyed\n"; return 0; }
int rkllm_is_running(LLMHandle) { return stub.running ? 1 : 0; }
int rkllm_abort(LLMHandle) {
    ++stub.abort;
    stub.aborted = true;
    return stub.fault == Fault::AbortFailure ? -3 : 0;
}
int rkllm_run(LLMHandle, RKLLMInput* input, RKLLMInferParam* infer, void* userdata) {
    ++stub.run;
    stub.events.push_back(std::string(input->prompt_input).empty() ? "bootstrap" : "request");
    stub.running = true;
    stub.independent = stub.independent && input->input_type == RKLLM_INPUT_PROMPT &&
        std::string(input->role) == "user" && !input->enable_thinking && infer->keep_history == 0 &&
        infer->max_new_tokens == firefly::RkllmResponseTokens && infer->mode == RKLLM_INFER_GENERATE && !infer->prompt_cache_params;
    if (stub.run == 1) stub.bootstrap = std::string(input->prompt_input).empty();
    else stub.independent = stub.independent && stub.clear >= stub.run;
    void* context = userdata ? userdata : stub.callbacks.result_userdata;
    RKLLMResult result{};
    result.text = R"({"actionId":"action_open_entrance","targetId":"device_entrance_door","arguments":{}})";
    result.perf.prefill_tokens = stub.run == 1 ? 565 : 30;
    if (std::string(input->prompt_input).empty()) result.perf.prefill_tokens = stub.refreshBootstrapTokens;
    result.perf.generate_tokens = 25;
    if (stub.fault == Fault::Context)
        result.perf.prefill_tokens = stub.run == 1 ? firefly::RkllmBootstrapCeiling + 1 : firefly::RkllmRequestReserve + 1;
    if (stub.fault == Fault::OutputLimit) result.perf.generate_tokens = firefly::RkllmResponseTokens - 1;
    if (stub.fault == Fault::MissingPerf) result.perf = {};
    const auto emit = [&](LLMCallState state) { stub.callbacks.result_callback(&result, context, state); };
    if (stub.fault == Fault::Timeout || stub.fault == Fault::AbortFailure) {
        while (!stub.aborted) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    emit(RKLLM_RUN_WAITING);
    if (stub.fault == Fault::NullText) result.text = nullptr;
    std::string large(5000, 'x');
    if (stub.fault == Fault::Overflow) result.text = large.c_str();
    emit(RKLLM_RUN_NORMAL);
    if (stub.fault == Fault::State) emit(static_cast<LLMCallState>(99));
    if (stub.fault == Fault::Error) emit(RKLLM_RUN_ERROR);
    if (stub.fault != Fault::MissingFinish) emit(RKLLM_RUN_FINISH);
    stub.running = false;
    return stub.fault == Fault::Run ? -1 : 0;
}
}
