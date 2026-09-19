#include "firefly/CommandEngine.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/llm/LlmContext.h"
#include <iostream>

using namespace firefly;
using namespace firefly::actions;
struct LiteralModel final : LlmEngine {
    std::string raw;
    int calls = 0;
    std::string infer(std::string_view) override { ++calls; return raw; }
};
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Repository root required");
        const runtime::DomainView domain(config::ConfigLoader::fromFile(std::string(argv[1]) + "/config/current-m4.json"));
        const DeterministicParser parser(domain);
        LiteralModel model;
        CommandEngine engine(model, parser, domain);
        int checks = 0;
        const auto check = [&](bool condition) { ++checks; if (!condition) throw std::runtime_error("Pipeline assertion " + std::to_string(checks)); };
        const auto light = [](std::int64_t value) {
            return GenericAction("action_set_brightness", "device_kitchen_light", {{"brightness", IntegerArgument(value)}});
        };
        for (const auto* raw : {"L|K|40", "D", "U", " \n L | K | 4 0 \t"}) {
            model.raw = raw;
            const auto result = engine.process("Пожалуйста на кухне свет 40");
            check(result.invalidModelOutput && result.decodeStatus == llm::DecodeStatus::InvalidJson);
            check(std::holds_alternative<Unsupported>(result.action) && result.rawOutput == raw);
        }
        for (const CandidateResult& candidate : {CandidateResult{Unsupported{}}, CandidateResult{light(50)}, CandidateResult{light(0)},
                CandidateResult{GenericAction("action_open_entrance", "device_entrance_door", {})}}) {
            model.raw = llm::structuredResult(candidate);
            const auto result = engine.process("На кухне выключи свет на 50 процентов");
            check(!result.invalidModelOutput && result.candidate == candidate);
            check(std::holds_alternative<Unsupported>(result.action));
            check(result.safetyRejected == std::holds_alternative<GenericAction>(candidate));
        }
        model.raw = " \n" + llm::structuredResult(light(40)) + "\t";
        auto result = engine.process("Пожалуйста на кухне свет 40");
        check(result.action == CandidateResult{light(40)} && result.candidate == result.safeCandidate);
        check(result.rawOutput == model.raw && !result.safetyRejected && !result.invalidModelOutput);
        result = engine.process("Пожалуйста в спальне свет 40");
        check(result.decodeStatus == llm::DecodeStatus::Action && result.safetyRejected && std::holds_alternative<Unsupported>(result.action));
        model.raw = llm::structuredResult(light(101));
        result = engine.process("Пожалуйста на кухне свет 101");
        check(result.decodeStatus == llm::DecodeStatus::OutsideDomain && result.invalidModelOutput && std::holds_alternative<Unsupported>(result.action));
        model.raw = R"({"actionId":"action_set_brightness","targetId":"device_kitchen_light ","arguments":{"brightness":40}})";
        result = engine.process("Пожалуйста на кухне свет 40");
        check(result.decodeStatus == llm::DecodeStatus::UnknownTarget && std::holds_alternative<Unsupported>(result.action));
        const int before = model.calls;
        result = engine.process("На кухне поставь свет на 40 процентов");
        check(result.action == CandidateResult{light(40)} && result.decodeStatus == llm::DecodeStatus::NotAttempted && model.calls == before);
        std::cout << "StructuredPipeline: " << checks << " checks; literal JSON ingress, no compact or whitespace repair, safety/domain/fast-path separation\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
