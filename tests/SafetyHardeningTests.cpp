#include "firefly/ActionValidator.h"
#include "firefly/CommandEngine.h"
#include "firefly/DeterministicParser.h"
#include "firefly/SafetyGate.h"
#include <fstream>
#include <iostream>
#include <tuple>
#include <vector>

namespace {
using namespace firefly;
int checks = 0;
void check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}
class FakeLlm final : public LlmEngine {
public:
    std::string output;
    std::string received;
    int calls = 0;
    std::string infer(std::string_view input) override {
        ++calls;
        received = input;
        return output;
    }
};
// Explicit, immutable parser dependency for standalone core tests. The unchanged
// 6.4 suites and actual assistant runs separately cover the validated snapshot.
class TestLocations final : public ParserRoomDomain {
public:
    std::vector<ParserRoomLocation> parserLocations() const override {
        return {{"на кухне", 'K'}, {"в спальне", 'B'}, {"в гостиной", 'G'},
                {"в зале", 'G'}, {"в коридоре", 'C'}};
    }
};
std::string compact(char room, int value) {
    return "L|" + std::string(1, room) + "|" + std::to_string(value);
}
void gate(const std::string& input, const std::string& candidate, const std::string& expected) {
    const auto actual = SafetyGate::apply(input, candidate);
    check(actual == expected, "gate: " + input + " / " + candidate + " -> " + actual);
    check(actual == candidate || actual == "U", "gate must never repair a candidate");
    const auto action = ActionValidator::validate(actual);
    check((action ? toCompact(*action) : "U") == expected, "strict final action: " + input);
}
void rejectEveryCandidate(const std::string& input) {
    gate(input, "U", "U");
    gate(input, "D", "U");
    for (const char room : std::string("KBGC"))
        for (int value = 0; value <= 100; ++value) gate(input, compact(room, value), "U");
    for (const auto* malformed : {"", "L|K|101", "L|K|-1", " L|K|50", "L|K|50\nD"})
        gate(input, malformed, "U");
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Frozen DEV path required");
        TestLocations locations;
        const DeterministicParser parser(locations);
        parser.requireLegacyRoomDomain();
        FakeLlm fake;
        CommandEngine configured(fake, parser);
        CommandEngine reference(fake);
        const auto fallback = [&](const std::string& input, const std::string& raw, const std::string& expected) {
            fake.output = raw;
            for (auto* engine : {&configured, &reference}) {
                const int before = fake.calls;
                const auto result = engine->process(input);
                check(result.compact() == expected, "fallback: " + input + " / " + raw);
                check(result.path == CommandPath::LlmFallback && fake.calls == before + 1,
                      "one unchanged fallback call, no inference bypass workaround");
                check(result.rawOutput == raw && fake.received == input && result.error.empty(),
                      "raw/original preserved; semantic rejection is not an engine error");
            }
        };

        // Read, do not copy or change, the exact frozen contradiction fixtures.
        std::ifstream dev(argv[1]);
        check(bool(dev), "DEV fixture readable");
        std::string line;
        int rows = 0, contradictions = 0;
        while (std::getline(dev, line)) {
            if (line.empty() || line.front() == '#') continue;
            ++rows;
            const auto first = line.find('\t'), second = line.find('\t', first + 1), third = line.find('\t', second + 1);
            const auto id = line.substr(0, first), category = line.substr(first + 1, second - first - 1);
            const auto expected = line.substr(second + 1, third - second - 1), input = line.substr(third + 1);
            if (id == "contradiction02") {
                check(input == "На кухне выключи свет на 50 процентов" && expected == "U", "exact reported fixture");
                fallback(input, "U", "U");
                fallback(input, "L|K|50", "U");
                fallback(input, " \n L | K | 5 0 \t", "U");
                // A model selecting the other conflicting request is not a repair.
                fallback(input, "L|K|0", "U");
                gate(input, "L|K|00050", "U");
            }
            if (category == "contradiction") {
                ++contradictions;
                check(expected == "U" && !parser.parseChecked(input), "contradictions remain unresolved by parser");
                rejectEveryCandidate(input);
                for (const auto* candidate : {"U", "D", "L|K|0", "L|K|50", "L|B|100", "L|G|20", "L|C|80"})
                    fallback(input, candidate, "U");
            }
            fake.output = expected;
            check(configured.process(input).compact() == expected, "all DEV controls through actual shared gate");
        }
        check(rows == 48 && contradictions == 4, "complete frozen DEV and sibling contradictions");

        const std::vector<std::pair<std::string, char>> rooms{{"на кухне", 'K'}, {"в спальне", 'B'},
            {"в гостиной", 'G'}, {"в зале", 'G'}, {"в коридоре", 'C'}};
        for (const auto& [location, room] : rooms) {
            for (int value = 0; value <= 100; ++value) {
                // General state/value compatibility, including each boundary.
                for (const auto& [state, compatible] : std::vector<std::pair<std::string, bool>>{
                         {"выключи", value == 0}, {"включи", value != 0}}) {
                    const auto input = location + " " + state + " свет на " + std::to_string(value) + " процентов";
                    gate(input, compact(room, value), compatible ? compact(room, value) : "U");
                    if (!compatible) gate(input, compact(room, state == "выключи" ? 0 : 100), "U");
                }
                for (const auto& [state, fixed] : std::vector<std::pair<std::string, int>>{{"максимум", 100}, {"наполовину", 50}}) {
                    const auto input = location + " свет " + state + " " + std::to_string(value) + "%";
                    gate(input, compact(room, value), value == fixed ? compact(room, value) : "U");
                    if (value != fixed) gate(input, compact(room, fixed), "U");
                }
                gate("выключи свет " + location, compact(room, value), value == 0 ? compact(room, 0) : "U");
                // Numeric-only commands retain the frozen fast path and values.
                const int before = fake.calls;
                check(configured.process(location + " свет " + std::to_string(value)).compact() == compact(room, value), "numeric control");
                check(before == fake.calls, "numeric fast path bypasses model");
            }
            for (const auto* verb : {"выключи", "выключите", "выключить", "выключай", "выключайте"}) {
                for (const auto& input : {"Пожалуйста, " + std::string(verb) + " свет " + location + " на 1%",
                         location + " " + verb + " освещение на пятьдесят процентов!",
                         "Полностью " + std::string(verb) + " свет " + location + " наполовину",
                         location + " " + verb + " свет на максимум"}) {
                    rejectEveryCandidate(input);
                    fallback(input, compact(room, 50), "U");
                }
                gate(std::string(verb) + " свет " + location + " на ноль процентов", compact(room, 0), compact(room, 0));
            }
            for (const auto& [input, value] : std::vector<std::pair<std::string, int>>{
                     {"выключи свет " + location, 0}, {"включи свет " + location, 100},
                     {location + " свет на максимум", 100}, {location + " свет наполовину", 50}}) {
                const int before = fake.calls;
                const auto result = configured.process(input);
                check(result.path == CommandPath::FastPath && result.compact() == compact(room, value) && before == fake.calls,
                      "state fast path reaches shared gate with no LLM");
                gate(input, compact(room, value), compact(room, value));
            }
        }
        for (const auto* input : {"  На КуХнЕ ВЫКЛЮЧИ СВЕТ НА 50 ПРОЦЕНТОВ! ",
                 "Выключи свет на кухне на 50", "Выключи свет на кухне 50", "На кухне выключи свет 50%",
                 "Выключи свет на кухне на 50процентов", "Выключи свет на кухне на 0.0%",
                 "Выключи свет на кухне на -1%", "Выключи свет на кухне на 101%",
                 "Выключи свет на кухне на неизвестно сколько процентов", "Включи и выключи свет на кухне",
                 "Свет на кухне наполовину и на максимум", "Выключи свет на кухне на 50% и открой входную дверь"})
            rejectEveryCandidate(input);

        // Close positive controls; state vocabulary in another grammatical role
        // must not trigger the new detector. These exercise the gate, not new grammar.
        for (const auto& [input, value] : std::vector<std::pair<std::string, int>>{
                 {"Выключи свет на кухне на 0%", 0}, {"Выключи свет на кухне на нуль процентов", 0},
                 {"Включи свет на кухне на 1 процент", 1}, {"Включи свет на кухне на пятьдесят процентов", 50},
                 {"Свет на кухне наполовину, 50 процентов", 50}, {"Свет на кухне наполовину пятьдесят процентов", 50},
                 {"Свет на кухне на максимум 100%", 100}, {"Свет на кухне на максимум сто процентов", 100},
                 {"Включи свет на кухне на девяносто девять процентов", 99},
                 {"На кухне свет был выключен, поставь яркость на 50 процентов", 50},
                 {"Установи свет на кухне на 50 процентов с выключателя", 50},
                 {"После выключения поставь свет на кухне на 50 процентов", 50}}) {
            gate(input, compact('K', value), compact('K', value));
            fallback(input, compact('K', value), compact('K', value));
        }
        // A time quantity is not brightness; this preserves gate behavior only,
        // and does not add timed commands to the parser/product.
        gate("выключи свет на кухне через 50 секунд", "L|K|0", "L|K|0");
        for (const auto* input : {"выключи телевизор в гостиной", "в детской выключи свет", "не включи свет на кухне",
                                 "выключи свет через 50 секунд", "на кухне свет 40 или 50 процентов"})
            fallback(input, "U", "U");
        gate("открой входную дверь", "D", "D");
        gate("Открыть дверь на входе", "D", "D");
        gate("открой дверь", "D", "U");
        std::cout << "SafetyHardening: " << checks << " checks passed; four frozen contradictions, all candidate rooms/levels, five locations, paraphrases, controls and both engine routes\n";
    } catch (const std::exception& error) {
        std::cerr << "SafetyHardening failed after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
