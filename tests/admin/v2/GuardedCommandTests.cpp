#include "../Support.h"
#include <iostream>

using namespace firefly;
using namespace admin_test;
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("root required");
        Model model;
        admin::RuntimeGenerations runtime(model);
        runtime.apply(catalog(base(argv[1])));
        const auto active = runtime.current();
        const auto guardFor = [&](int brightness) {
            return admin::CommandGuard{active->domain.snapshot()->contentHash(),
                actions::GenericAction("action_set_brightness", "device_kitchen_light", {{"brightness", actions::Scalar(std::int64_t(brightness))}})};
        };
        auto check = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
        auto guard = guardFor(40);
        auto result = runtime.beginRequest()->process("На кухне поставь свет на 40 процентов", execution::ExecutionIntent::Production, "test", &guard);
        check(result.guardError.empty(), "deterministic admitted");
        check(result.command.path == CommandPath::FastPath, "fast route");
        check(!result.command.safetyRejected, "safety preserved");
        auto wrong = guardFor(20);
        result = runtime.beginRequest()->process("На кухне поставь свет на 40 процентов", execution::ExecutionIntent::Production, "test", &wrong);
        check(result.guardError == "expected_action_mismatch" && !result.execution, "mismatch before executor");
        guard = guardFor(0);
        result = runtime.beginRequest()->process("Включи свет на кухне на ноль процентов", execution::ExecutionIntent::Production, "test", &guard);
        check(!result.guardError.empty() && !result.execution, "unsafe rejected");
        result = runtime.beginRequest()->process("Сделай красиво", execution::ExecutionIntent::Production, "test", &guard);
        check(result.guardError == "deterministic_required" && !result.execution, "no fallback admission");
        check(model.inferences == 0, "guarded path must never call model");
        const auto* door = active->domain.entranceTarget();
        const admin::CommandGuard doorGuard{active->domain.snapshot()->contentHash(), actions::GenericAction(door->actionIds.front(), door->id, {})};
        result = runtime.beginRequest()->process("Открой входную дверь", execution::ExecutionIntent::Production, "test", &doorGuard);
        check(result.guardError == "explicit_manual_intent_required" && !result.execution, "door text-only manual lane");
        auto stale = guardFor(40); stale.expectedHash = std::string(64, '0');
        try { runtime.beginRequest()->process("На кухне поставь свет на 40 процентов", execution::ExecutionIntent::Production, "test", &stale); throw std::runtime_error("stale accepted"); }
        catch (const config::ConfigError& error) { check(error.code == "revision_conflict", "stale code"); }
        check(model.inferences == 0, "no inference after all guards");
        std::cout << "PASS guarded admission: deterministic/SafetyGate/domain/exact action, stale, door, zero LLM calls\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
