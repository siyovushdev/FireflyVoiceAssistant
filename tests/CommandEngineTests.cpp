#include "firefly/CommandEngine.h"
#include <iostream>
#include <stdexcept>
#include <tuple>
#include <vector>

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
    firefly::CommandEngine engine(fake);
    for (const auto& [text, expected] : std::vector<std::pair<std::string, std::string>>{
        {"На кухне поставь свет на 40 процентов", "L|K|40"},
        {"на кухне сделай свет наполовину", "L|K|50"},
        {"в коридоре сделай свет наполовину", "L|C|50"},
        {"В зале включи свет", "L|G|100"}}) {
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
        {"Какая сегодня погода?", "U", "U"}, {"Какая сегодня погода?", "", "U"},
        {"Какая сегодня погода?", "```D```", "U"}, {"", "U", "U"}}) {
        fake.output = raw;
        const int before = fake.calls;
        const auto result = engine.process(text);
        check(result.compact() == expected, "fallback result");
        check(result.path == firefly::CommandPath::LlmFallback && fake.calls == before + 1, "one LLM call");
        check(fake.received == text, "original input retained");
    }
    fake.fail = true;
    bool threw = false;
    try { (void)engine.process("Какая сегодня погода?"); }
    catch (const std::runtime_error&) { threw = true; }
    check(threw, "runtime errors propagate without action");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
