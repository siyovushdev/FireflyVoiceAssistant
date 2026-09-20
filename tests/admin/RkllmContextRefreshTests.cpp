#include "firefly/RkllmEngine.h"
#include "FakeRkllmRuntime.h"
#include <iostream>
using namespace firefly_test;
int main() {
    int checks = 0, failures = 0;
    const auto check = [&](bool pass, const char* name) { ++checks; if (!pass) { ++failures; std::cerr << "FAIL " << name << '\n'; } };
    firefly::RkllmConfig config;
    config.modelPath = "fake"; config.systemPrompt = "frozen"; config.requestTokenReserve = 128;
    {
        firefly::RkllmEngine engine(config);
        check(engine.ready() && stub.init == 1, "initial context ready");
        stub.events.clear();
        engine.setSystemPrompt("next context");
        check(stub.events == std::vector<std::string>{"clear:0", "template", "bootstrap", "clear:1"}, "full clear before refresh bootstrap, then keep system");
        check(stub.init == 1 && stub.destroy == 0 && engine.ready(), "refresh reuses model handle");
        stub.events.clear(); engine.setSystemPrompt("next context");
        check(stub.events.empty(), "unchanged context no extra cache operation");
        (void)engine.infer("Открой входную дверь");
        check(stub.events == std::vector<std::string>{"clear:1", "request"}, "normal request lifecycle unchanged");
        stub.events.clear(); stub.rejectTemplate = "reject_context";
        bool rejected = false;
        try { engine.setSystemPrompt("reject_context"); } catch (...) { rejected = true; }
        check(rejected && engine.ready() && stub.system.find("next context") != std::string::npos, "failed context restores old");
        check(stub.events == std::vector<std::string>{"clear:0", "template", "clear:0", "template", "bootstrap", "clear:1"}, "rollback lifecycle exact");
        check(stub.init == 1 && !engine.infer("Открой входную дверь").empty(), "old context inference after rejection without reload");
        stub.rejectTemplate.clear();
        // Exact bootstrap ceiling 768 with 128 request + 128 output reservations.
        stub.refreshBootstrapTokens = 768; engine.setSystemPrompt("fits boundary");
        check(engine.ready(), "bootstrap limit accepted");
        stub.refreshBootstrapTokens = 769; rejected = false;
        try { engine.setSystemPrompt("too many actual tokens"); } catch (...) { rejected = true; }
        check(rejected && !engine.ready(), "bad token admission and failed restore do not claim ready");
        rejected = false;
        try { (void)engine.infer("Открой входную дверь"); } catch (...) { rejected = true; }
        check(rejected, "no stale response after fatal restoration");
    }
    check(stub.destroy == 1, "single owned model destroyed");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
