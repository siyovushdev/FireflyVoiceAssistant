#include "TypedRuntimeChecks.h"
#include "firefly/voice/VoiceProcessor.h"
#include <array>
using namespace runtime_test;
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Repository root required");
        Runtime runtime(config::ConfigLoader::fromFile(std::string(argv[1]) + "/config/current-m4.json"));
        static_assert(std::is_same_v<decltype(CommandResult{}.action), CandidateResult>);
        static_assert(std::is_same_v<decltype(voice::VoiceResult{}.action), CandidateResult>);
        const std::array<std::pair<std::string, std::string>, 4> rooms{{
            {"на кухне", "device_kitchen_light"}, {"в спальне", "device_bedroom_light"},
            {"в гостиной", "device_living_light"}, {"в коридоре", "device_corridor_light"}}};
        int lights = 0;
        for (const auto& [location, target] : rooms) {
            for (std::int64_t brightness = 0; brightness <= 100; ++brightness) {
                const auto expected = light(target, brightness);
                const auto text = location + " свет " + std::to_string(brightness);
                const auto fast = runtime.engine.process(text);
                check(action(fast.action) == expected, "fast semantic tuple: actionId/targetId/int64 argument");
                check(fast.path == CommandPath::FastPath, "typed parser bypasses RKLLM");
                runtime.llm.raw = runtime.codec.render(expected);
                const auto fallback = runtime.engine.process("Пожалуйста, " + text);
                check(action(fallback.action) == expected && fallback.path == CommandPath::LlmFallback,
                      "one ingress decode leads to same typed final action");
                check(fallback.normalizedOutput == runtime.llm.raw && fallback.rawOutput == runtime.llm.raw,
                      "raw model diagnostics remain separate from canonical result");
                check(runtime.codec.render(fast.action) == runtime.codec.render(fallback.action), "external compact compatibility");
                ++lights;
            }
        }
        check(lights == 404, "all current semantic light actions exercised on both routes");
        runtime.llm.raw = "D";
        const auto door = runtime.engine.process("Открой входную дверь");
        check(action(door.action) == GenericAction("action_open_entrance", "device_entrance_door", {}),
              "door result owns explicit configured identity and no arbitrary argument");
        check(door.path == CommandPath::LlmFallback, "door still uses frozen RKLLM route");
        check(unsupported(runtime.engine.process("Открой дверь").action), "ambiguous door rejected even for D");
        for (const auto* raw : {"U", "D", "L|K|40"}) {
            runtime.llm.raw = raw;
            check(unsupported(runtime.engine.process("Какая сегодня погода").action), "unsupported input cannot acquire executable action");
        }
        for (const auto* raw : {"U", "L|K|50", "L|K|0", "D"}) {
            runtime.llm.raw = raw;
            check(unsupported(runtime.engine.process("На кухне выключи свет на 50 процентов").action),
                  "contradiction02 independent of model candidate variation");
        }
        check(action(runtime.engine.process("На кухне поставь свет на 40 процентов").action) == light("device_kitchen_light", 40),
              "required kitchen phrase");
        check(action(runtime.engine.process("В спальне выключи свет").action) == light("device_bedroom_light", 0),
              "required bedroom OFF phrase");
        for (const auto& [location, target] : rooms) {
            const auto candidate = light(target, 40);
            const auto& roomId = *runtime.domain.device(target)->roomId;
            for (const auto& [alias, mentionedRoom] : runtime.domain.snapshot()->mentionIndex()) {
                if (mentionedRoom != roomId) continue;
                check(SafetyGate::apply(location + " свет 40, " + alias, candidate, runtime.domain) == CandidateResult{candidate},
                      "repeated mentions or synonyms of the same room are not target ambiguity");
            }
            for (const auto& [otherLocation, otherTarget] : rooms) {
                if (target == otherTarget) continue;
                check(unsupported(SafetyGate::apply(location + " и " + otherLocation + " свет 40", candidate, runtime.domain)),
                      "multiple different target identities fail closed");
            }
        }
        // Frozen compatibility labels remain independent input to typed assertions.
        std::ifstream dev(std::string(argv[1]) + "/tests/data/dev.tsv");
        std::string line;
        int rows = 0;
        while (std::getline(dev, line)) {
            if (line.empty() || line.front() == '#') continue;
            std::istringstream fields(line);
            std::string id, category, expected, input;
            std::getline(fields, id, '\t'); std::getline(fields, category, '\t');
            std::getline(fields, expected, '\t'); std::getline(fields, input);
            runtime.llm.raw = expected;
            const auto result = runtime.engine.process(input);
            check(runtime.codec.render(result.action) == expected, "frozen DEV external result: " + id);
            const auto decoded = runtime.codec.decode(expected);
            check(result.action == std::get<CandidateResult>(decoded), "frozen DEV semantic result: " + id);
            ++rows;
        }
        check(rows == 48, "all existing DEV cases preserved");
        return finish("GenericRuntime");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
