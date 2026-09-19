#pragma once
#include <stddef.h> // Vendor header requires size_t to be declared first.
#include "rkllm.h"
#include <atomic>
namespace firefly_test {
enum class Fault { None, Run, Clear, State, Error, MissingFinish, NullText, Overflow, Timeout, AbortFailure,
                   Context, OutputLimit, CacheRead, CacheSize, MissingPerf };
struct Stub {
    RKLLMCallback callbacks{};
    Fault fault = Fault::None;
    std::atomic<bool> running{false}, aborted{false};
    int init = 0, run = 0, clear = 0, destroy = 0, abort = 0;
    bool settings = false, bootstrap = false, independent = true;
};
extern Stub stub;
}
