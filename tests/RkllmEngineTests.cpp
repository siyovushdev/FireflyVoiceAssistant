#include "firefly/RkllmEngine.h"
#include "firefly/CommandEngine.h"
#include "TestSupport.h"
#include "rkllm.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <stdexcept>

#include "FakeRkllmRuntime.h"
using namespace firefly_test;
int main() {
    int checks = 0, failures = 0;
    const auto check = [&](bool pass, const char* name) {
        ++checks;
        if (!pass) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
    };
    for (const auto fault : {Fault::None, Fault::Run, Fault::Clear, Fault::State, Fault::Error,
             Fault::MissingFinish, Fault::NullText, Fault::Overflow, Fault::Timeout, Fault::AbortFailure}) {
        stub.fault = Fault::None;
        stub.init = stub.run = stub.clear = stub.destroy = stub.abort = 0;
        stub.aborted = false;
        stub.independent = true;
        {
            firefly::RkllmConfig config;
            config.modelPath = "fake";
            config.systemPrompt = "frozen";
            config.requestTimeout = std::chrono::milliseconds(30);
            firefly::RkllmEngine llm(config);
            test::Engine engine(llm);
            check(stub.settings && stub.bootstrap && stub.run == 1 && stub.clear == 1, "bootstrap and frozen configuration");
            check(engine.process("Открой входную дверь").compact() == "D", "successful first action");
            stub.fault = fault;
            const auto result = engine.process("Открой входную дверь");
            check(result.compact() == (fault == Fault::None ? "D" : "U"), "fault fails closed");
            if (fault != Fault::None) {
                check(!result.error.empty() && result.rawOutput.empty(), "failed output discarded");
                const int runs = stub.run;
                stub.fault = Fault::None;
                check(engine.process("Открой входную дверь").compact() == "U" && stub.run == runs,
                      "faulted engine stays not ready, no stale D");
            }
            check(stub.independent, "independent request KV lifecycle");
            if (fault == Fault::Timeout || fault == Fault::AbortFailure)
                check(stub.abort == 1 && !stub.running, "abort joined before returning");
        }
        check(stub.destroy == 1, "RAII destroys once");
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
