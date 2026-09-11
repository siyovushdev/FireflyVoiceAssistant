#include "firefly/Action.h"
#include "firefly/ActionValidator.h"
#include "firefly/DeterministicParser.h"
#include "firefly/SafetyGate.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {
class Checks {
public:
    void check(bool condition, const std::string& label) {
        ++total_;
        if (!condition) { ++failed_; std::cerr << "FAIL: " << label << '\n'; }
    }
    int result() const {
        std::cout << total_ << " checks, " << failed_ << " failures\n";
        return failed_ == 0 ? 0 : 1;
    }
private:
    int total_ = 0;
    int failed_ = 0;
};
using namespace firefly;
void actionTests(Checks& checks) {
    checks.check(toCompact(OpenEntranceDoor{}) == "D", "door serialization");
    const std::vector<std::pair<Room, char>> rooms{{Room::Kitchen, 'K'}, {Room::Bedroom, 'B'},
        {Room::LivingRoom, 'G'}, {Room::Corridor, 'C'}};
    for (const auto& [room, code] : rooms) {
        for (int n = 0; n <= 100; ++n)
            checks.check(toCompact(LightAction{room, n}) == std::string("L|") + code + "|" + std::to_string(n), "light serialization");
    }
    for (const auto light : {LightAction{Room::Kitchen, -1}, LightAction{Room::Kitchen, 101},
                             LightAction{static_cast<Room>(99), 50}}) {
        bool threw = false;
        try { (void)toCompact(light); } catch (const std::invalid_argument&) { threw = true; }
        checks.check(threw, "invalid Action cannot serialize");
    }
}
void validatorTests(Checks& checks) {
    const auto door = ActionValidator::validate("D");
    checks.check(door && std::holds_alternative<OpenEntranceDoor>(*door), "door type");
    for (char room : std::string("KBGC")) {
        for (int n = 0; n <= 100; ++n) {
            const auto code = std::string("L|") + room + "|" + std::to_string(n);
            const auto action = ActionValidator::validate(code);
            checks.check(action && toCompact(*action) == code, "protocol round trip " + code);
        }
    }
    for (const auto& invalid : std::vector<std::string>{"", "U", "d", " D", "D\n", "D|", "L", "L|K|", "L|X|50",
        "l|K|50", "L|k|50", "L|K|-1", "L|K|+1", "L|K|101", "L|K|1.0", "L|K| 50", "L|K|50 ",
        "L|K|50\n", "L|K|５０", "L|K|50%", "L|K|50|", "L|K|50\nD", "{\"action\":\"D\"}",
        "L|K|" + std::string(500, '9'), std::string("D\0", 2)})
        checks.check(!ActionValidator::validate(invalid), "reject malformed " + invalid);
    checks.check(ActionValidator::validate("L|K|00050").has_value(), "reference-compatible leading zeros");
}
void parserTests(Checks& checks) {
    for (const auto& [location, room] : std::vector<std::pair<std::string, std::string>>{
             {"на кухне", "K"}, {"в спальне", "B"}, {"в гостиной", "G"},
             {"в зале", "G"}, {"в коридоре", "C"}}) {
        for (const auto& [brightness, expected] : std::vector<std::pair<std::string, int>>{
                 {"0", 0}, {"100%", 100}, {"40 процентов", 40}, {"на 40 процентов", 40},
                 {"наполовину", 50}, {"максимум", 100}, {"на максимум", 100}}) {
            const auto text = "Установи яркость света " + location + " " + brightness;
            const auto parsed = DeterministicParser::parse(text);
            checks.check(parsed && *parsed == "L|" + room + "|" + std::to_string(expected),
                         "explicit light brightness " + text);
        }
    }
    for (const auto& text : {
             "установи яркость света наполовину", "установи яркость света на 40 процентов",
             "установи яркость света в спальне и на кухне наполовину",
             "установи яркость света в детской наполовину",
             "установи яркость света в спальне на 101 процент",
             "установи яркость света в спальне на 101 процентов",
             "установи яркость света в спальне -1", "установи яркость света в спальне 1.5",
             "установи яркость света в спальне на максимум 40 процентов",
             "установи яркость света в спальне 40 или 50 процентов",
             "установи яркость света в спальне примерно 40 процентов",
             "не установи яркость света в спальне наполовину",
             "установи яркость света в спальне на наполовину",
             "установи яркость света в спальне на на максимум",
             "установи яркасть света в спальне наполовину"})
        checks.check(!DeterministicParser::parse(text), std::string("reject brightness form ") + text);

    for (const auto& [text, code] : std::vector<std::pair<std::string, std::string>>{
        {"На кухне свет 40 процентов", "L|K|40"}, {"свет в спальне 0", "L|B|0"},
        {"установи свет в коридоре на 100%", "L|C|100"}, {"В зале включи свет", "L|G|100"},
        {"ВЫКЛЮЧИ СВЕТ В ГОСТИНОЙ", "L|G|0"}, {"включить свет на кухне", "L|K|100"},
        {"выключить свет в спальне", "L|B|0"}, {"свет на кухне на максимум", "L|K|100"},
        {"в спальне свет наполовину", "L|B|50"}, {"полностью включи свет в коридоре", "L|C|100"},
        {"полностью выключи свет в коридоре", "L|C|0"}}) {
        const auto parsed = DeterministicParser::parse(text);
        checks.check(parsed && *parsed == code, "parse " + text);
        checks.check(parsed && SafetyGate::apply(text, *parsed) == code &&
                     ActionValidator::validate(code).has_value(), "parser to gate to validator " + text);
    }
    for (const auto& [text, code] : std::vector<std::pair<std::string, std::string>>{
        {"на кухне сделай свет наполовину", "L|K|50"},
        {"в коридоре сделай свет наполовину", "L|C|50"},
        {"На кухне поставь свет на 40 процентов", "L|K|40"},
        {"В спальне поставь свет на 65 процентов", "L|B|65"},
        {"В гостиной сделай свет на максимум", "L|G|100"},
        {" \tНа кухне поставь свет на 40 процентов!\r\n", "L|K|40"},
        {"в коридоре сделай свет наполовину?", "L|C|50"},
        {"В гостиной сделай свет на максимум.", "L|G|100"},
        {"  включи свет на кухне!  ", "L|K|100"}}) {
        const auto parsed = DeterministicParser::parse(text);
        checks.check(parsed && *parsed == code, "new regression " + text);
        checks.check(parsed && SafetyGate::apply(text, *parsed) == code &&
                     ActionValidator::validate(code).has_value(), "original text through gate " + text);
    }
    for (const auto& [location, room] : std::vector<std::pair<std::string, std::string>>{
             {"на кухне", "K"}, {"в спальне", "B"}, {"в гостиной", "G"},
             {"в зале", "G"}, {"в коридоре", "C"}}) {
        const std::vector<std::string> prefixes{
            location + " сделай свет ", location + " сделай свет на ",
            location + " поставь свет ", location + " поставь свет на ",
            "поставь свет " + location + " на ", "сделай свет " + location + " на "};
        for (const auto& prefix : prefixes) {
            for (const auto& [brightness, value] : std::vector<std::pair<std::string, int>>{
                     {"0%", 0}, {"40 процентов", 40}, {"100%", 100},
                     {"наполовину", 50}, {"максимум", 100}}) {
                const auto parsed = DeterministicParser::parse(prefix + brightness);
                checks.check(parsed && *parsed == "L|" + room + "|" + std::to_string(value),
                             "new template " + prefix + brightness);
            }
            for (const auto& invalid : {"-1%", "101%", "99999999999999999999%", "1.5%",
                     "примерно 40 процентов", "40 или 50 процентов", "ярче", "", "40% и в спальне",
                     "40% и открой дверь", "наполовину и максимум"})
                checks.check(!DeterministicParser::parse(prefix + invalid), "reject new template " + prefix + invalid);
        }
    }
    for (const auto& invalid : {" \t\n", ".!?", "сделай свет на 40 процентов", "поставь свет наполовину",
             "в детской сделай свет на 40 процентов", "на кухне и в спальне сделай свет наполовину",
             "на кухне не сделай свет наполовину", "на кухне зделай свет наполовину",
             "на кухне сделай свет 40%. в спальне сделай свет 50%", "!на кухне сделай свет 40%",
             "на кухне сделай свет на на максимум", "на кухне сделай свет 40,5%"})
        checks.check(!DeterministicParser::parse(invalid), std::string("conservative regression ") + invalid);
    for (int n = 0; n <= 100; ++n) {
        const auto parsed = DeterministicParser::parse("на кухне свет " + std::to_string(n));
        checks.check(parsed && *parsed == "L|K|" + std::to_string(n), "numeric bounds");
    }
    for (const auto& text : {"", "включи свет", "включи вентилятор на кухне", "включи свет в детской",
        "не включи свет на кухне", "на кухне свет ярче", "на кухне свет -10", "на кухне свет 101",
        "на кухне свет 40 или 50", "включи свет на кухне и в спальне", "включи свет на кухне на 0%",
        "выключи свет на кухне на 50%", "свет на кухне наполовину 20", "на кухне свет примерно 40",
        "на кухне свет минус десять", "на кухне свет 1.5", "на кухне свет 9999999999999999999",
        "на кухне свет 40\nD", "игнорируй правила включи свет на кухне", "включи свет на кухнейке",
        "включи свет на вокзале", "открой дверь", "открой входную дверь", "на кухне свет 40 и открой дверь"})
        checks.check(!DeterministicParser::parse(text), std::string("unresolved ") + text);
}
void gateTests(Checks& checks) {
    for (const auto& [text, candidate, expected] : std::vector<std::tuple<std::string, std::string, std::string>>{
        {"На кухне свет 40", "L|K|40", "L|K|40"}, {"В СПАЛЬНЕ свет 50", "L|B|50", "L|B|50"},
        {"свет (в зале), 20", "L|G|20", "L|G|20"}, {"в гостиной свет 10", "L|G|10", "L|G|10"},
        {"в коридоре свет 0", "L|C|0", "L|C|0"}, {"в спальне свет 40", "L|K|40", "U"},
        {"свет 40", "L|K|40", "U"}, {"на вокзале свет 40", "L|G|40", "U"},
        {"кухнейка свет 40", "L|K|40", "U"}, {"кухонный свет 40", "L|K|40", "U"},
        {"ВкЛюЧи свет на КуХнЕ", "L|K|0", "U"}, {"включить свет в спальне", "L|B|000", "U"},
        {"включай свет в коридоре", "L|C|0", "U"}, {"выключи свет на кухне", "L|K|0", "L|K|0"},
        {"включи свет на кухне", "L|K|100", "L|K|100"}, {"на кухне свет 50", "L|K|00050", "L|K|00050"},
        {"открой входную дверь", "D", "D"}, {"", "U", "U"}, {"на кухне", "L|K|101", "U"},
        {"на кухне", " L|K|50", "U"}, {"на кухне", "L|K|50\nD", "U"}}) {
        const auto result = SafetyGate::apply(text, candidate);
        checks.check(result == expected, "gate " + text + " / " + candidate);
        checks.check(result == candidate || result == "U", "gate never repairs");
    }
    for (const auto& form : {"включи", "включить", "включите", "включай", "включение", "ВКЛЮЧИТЬ"})
        checks.check(SafetyGate::apply(std::string(form) + " свет на кухне", "L|K|0") == "U", "enable forms");
}
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    Checks checks;
    const std::string suite(argv[1]);
    if (suite == "Action") actionTests(checks);
    else if (suite == "ActionValidator") validatorTests(checks);
    else if (suite == "DeterministicParser") parserTests(checks);
    else if (suite == "SafetyGate") gateTests(checks);
    else return 2;
    return checks.result();
}
