#include "TestSupport.h"
#include "firefly/CommandEngine.h"
#include <iostream>
#include <stdexcept>
#include <tuple>
#include <vector>
#include <sstream>
#include "firefly/InputPolicy.h"

class FakeLlm final : public firefly::LlmEngine {
public:
    std::string output;
    std::string received;
    int calls = 0;
    bool fail = false;
    std::string infer(std::string_view original) override {
        ++calls;
        received = original;
        if (fail) throw std::runtime_error("fake runtime failure");
        return output;
    }
};
int main() {
    int checks = 0;
    int failures = 0;
    const auto check = [&](bool passed, const std::string& name) {
        ++checks;
        if (!passed) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
    };
    FakeLlm fake;
    test::Engine engine(fake);
    for (const auto& [text, expected] : std::vector<std::pair<std::string, std::string>>{
        {"На кухне поставь свет на 40 процентов", "L|K|40"},
        {"на кухне сделай свет наполовину", "L|K|50"},
        {"в коридоре сделай свет наполовину", "L|C|50"},
        {"В зале включи свет", "L|G|100"},
        {"Установи яркость света в спальне наполовину", "L|B|50"},
        {"Установи яркость света на кухне на 40 процентов", "L|K|40"},
        {"Установи яркость света в коридоре на максимум", "L|C|100"}}) {
        const auto result = engine.process(text);
        check(result.compact() == expected, "fast result");
        check(result.path == firefly::CommandPath::FastPath && fake.calls == 0, "bypass LLM");
    }
    for (const auto& [text, raw, expected] : std::vector<std::tuple<std::string, std::string, std::string>>{
        {"Пожалуйста, свет на кухне 40 процентов", "L|K|40", "L|K|40"},
        {"Пожалуйста, свет на кухне 40 процентов", " \n L | K | 4 0 \t", "L|K|40"},
        {"Пожалуйста, свет на кухне 40 процентов", "L|B|40", "U"},
        {"Свет на максимум", "L|K|100", "U"},
        {"  В спальне ВкЛюЧи свет на 0 процентов!  ", "L|B|0", "U"},
        {"Пожалуйста, свет на кухне 40 процентов", "L|K|101", "U"},
        {"Пожалуйста, свет на кухне 40 процентов", "L|K|40 extra", "U"},
        {"Пожалуйста, свет на кухне 40 процентов", "L|K|40\nD", "U"},
        {"Открой входную дверь", "\nD\n", "D"},
        {"дверь", "D", "U"}, {"двер", "D", "U"},
        {"открой дверь", "D", "U"}, {"открыть входную дверь", "D", "D"},
        {"открой входную тверь", "D", "U"},
        {"Открыть дверь на входе", "D", "D"},
        {"Открой дверь на входе", "D", "D"},
        {"открой тверь на входе", "D", "U"},
        {"Какая сегодня погода?", "U", "U"}, {"Какая сегодня погода?", "", "U"},
        {"Какая сегодня погода?", "```D```", "U"}}) {
        fake.output = raw;
        const int before = fake.calls;
        const auto result = engine.process(text);
        check(result.compact() == expected, "fallback result");
        check(result.path == firefly::CommandPath::LlmFallback && fake.calls == before + 1, "one LLM call");
        check(fake.received == text, "original input retained");
    }
    for (const auto& [location, room] : std::vector<std::pair<std::string,std::string>>{
        {"на кухне", "K"}, {"в спальне", "B"}, {"в гостиной", "G"}, {"в коридоре", "C"}}) {
        for (int n : {0, 1, 50, 99, 100}) {
            const int before = fake.calls;
            const auto result = engine.process("  " + location + " свет " + std::to_string(n) + "%!?  ");
            check(result.compact() == "L|" + room + "|" + std::to_string(n), "fast numeric matrix");
            check(fake.calls == before && result.path == firefly::CommandPath::FastPath, "fast matrix bypass");
        }
        for (const auto& [phrase, n] : std::vector<std::pair<std::string,int>>{
            {"включи свет " + location, 100}, {"выключи свет " + location, 0},
            {"свет " + location + " на максимум", 100}, {location + " сделай свет наполовину", 50}}) {
            const int before = fake.calls;
            check(engine.process(phrase).compact() == "L|" + room + "|" + std::to_string(n), "fast semantic matrix");
            check(fake.calls == before, "semantic matrix bypass");
        }
    }
    const int mixedBefore = fake.calls;
    check(engine.process(" \tВкЛюЧи свет На КуХнЕ! ").compact() == "L|K|100" && fake.calls == mixedBefore, "mixed case bypass");
    fake.output = "U";
    for (const auto& text : {"Включи свет", "На кухне и в спальне свет 50", "на кухне свет 101",
        "на кухне свет -1", "выключи свет на кухне на 40 процентов", "свет сделай поставь"}) {
        const int before = fake.calls;
        check(engine.process(text).compact() == "U" && fake.calls == before + 1, "unresolved routed once");
    }
    for (const auto& text : std::vector<std::string>{"", " \t", std::string(193, 'a'),
            std::string("a\0b", 3), std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3),
            std::string("\xf4\x90\x80\x80", 4), std::string("\xd0", 1), std::string("\x01", 1)}) {
        const int before = fake.calls;
        const auto result = engine.process(text);
        check(result.compact() == "U" && result.path == firefly::CommandPath::InputRejected && fake.calls == before,
              "invalid input bypasses both engines");
    }
    check(firefly::validInput(std::string(192, 'a')), "byte limit inclusive");
    check(firefly::validInput("Привет"), "valid Russian UTF-8");
    std::istringstream lines(std::string(5000, 'x') + "\nна кухне свет 1\n");
    std::string line;
    check(firefly::readInputLine(lines, line) && line.size() == 193, "bounded reader drains oversized line");
    check(firefly::readInputLine(lines, line) && line == "на кухне свет 1", "next line intact");
    check(!firefly::readInputLine(lines, line), "EOF");
    fake.output = "D";
    check(engine.process("Открой входную дверь").compact() == "D", "prime successful output");
    fake.fail = true;
    const auto failed = engine.process("Открой входную дверь");
    check(failed.compact() == "U" && !failed.error.empty() && failed.rawOutput.empty(), "failure never replays D");
    fake.fail = false;
    fake.output = "U";
    check(engine.process("Какая сегодня погода?").compact() == "U", "next request fresh");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
