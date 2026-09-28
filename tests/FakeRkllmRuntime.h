#pragma once
#include <stddef.h> // Vendor header requires size_t to be declared first.
#include "rkllm.h"
#include <atomic>
#include <string>
#include <vector>
namespace firefly_test {
enum class Fault { None, Run, Clear, State, Error, MissingFinish, NullText, Overflow, Timeout, AbortFailure,
                   Context, OutputLimit, CacheRead, CacheSize, MissingPerf };
struct Stub {
    RKLLMCallback callbacks{};
    Fault fault = Fault::None;
    std::atomic<bool> running{false}, aborted{false};
    int init = 0, run = 0, clear = 0, destroy = 0, abort = 0;
    bool settings = false, bootstrap = false, independent = true;
    // Explicit refresh-test controls; original fault suites keep defaults.
    std::string system, rejectTemplate, lastPrompt;
    int refreshBootstrapTokens = 565;
    int requestTokens = 30;
    bool failRestoration = false;
    std::vector<std::string> events;
    std::vector<int> outputBudgets;
};
extern Stub stub;
}
