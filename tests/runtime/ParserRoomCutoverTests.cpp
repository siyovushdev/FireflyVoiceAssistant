#include "firefly/CommandEngine.h"
#include "firefly/DeterministicParser.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/config/ConfigValidator.h"
#include "firefly/runtime/DomainView.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <type_traits>

namespace {
using namespace firefly;
static_assert(!std::is_default_constructible_v<DeterministicParser>);
static_assert(!std::is_copy_assignable_v<DeterministicParser>);
int checks = 0;
void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template<class Function> void mismatch(Function action) {
    bool rejected = false;
    try { action(); } catch (const RoomLookupMismatch&) { rejected = true; }
    check(rejected, "lookup disagreement must throw RoomLookupMismatch");
}
class FakeLlm final : public LlmEngine {
public:
    int calls = 0;
    std::string output = "U";
    std::string infer(std::string_view) override { ++calls; return output; }
};
struct Locations final : ParserRoomDomain {
    std::vector<ParserRoomLocation> entries;
    std::vector<ParserRoomLocation> parserLocations() const override { return entries; }
};
config::Room& room(config::Configuration& data, const std::string& id) {
    for (auto& entry : data.rooms) if (entry.id == id) return entry;
    throw std::runtime_error("Missing fixture room");
}
std::vector<std::string> numericCommands(const std::string& location, const std::string& value) {
    return {location + " свет " + value, "свет " + location + " " + value,
        "установи свет " + location + " на " + value,
        location + " сделай свет " + value, location + " сделай свет на " + value,
        location + " поставь свет " + value, location + " поставь свет на " + value,
        "сделай свет " + location + " на " + value, "поставь свет " + location + " на " + value,
        "установи яркость света " + location + " " + value,
        "установи яркость света " + location + " на " + value};
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Repository root required");
        const auto snapshot = config::ConfigLoader::fromFile(std::string(argv[1]) + "/config/current-m4.json");
        const runtime::DomainView domain(snapshot);
        const DeterministicParser parser(domain);
        parser.requireLegacyRoomDomain();
        FakeLlm llm;
        CommandEngine engine(llm, parser);
        const std::vector<std::pair<std::string, char>> locations{
            {"на кухне", 'K'}, {"в спальне", 'B'}, {"в гостиной", 'G'}, {"в зале", 'G'}, {"в коридоре", 'C'}};
        check(domain.parserLocations().size() == locations.size(), "exact migrated location count");
        for (const auto& [location, code] : locations) {
            const auto* canonical = domain.roomLocation(location);
            check(canonical && domain.room(canonical->id) == canonical, "canonical and alias identity agree");
            const auto* target = domain.legacyLightTarget(code);
            check(target && target->roomId == canonical->id, "canonical room maps to current target/code");
            for (int value = 0; value <= 100; ++value) {
                const auto expected = "L|" + std::string(1, code) + "|" + std::to_string(value);
                for (const auto& suffix : {"", "%", " процентов"}) {
                    for (const auto& input : numericCommands(location, std::to_string(value) + suffix)) {
                        check(parser.parseConfigured(input) == expected, "configured numeric grammar: " + input);
                        check(DeterministicParser::parse(input) == expected, "frozen numeric grammar: " + input);
                        const int before = llm.calls;
                        const auto result = engine.process(input);
                        check(result.compact() == expected && result.path == CommandPath::FastPath && llm.calls == before,
                              "new authority reaches unchanged gate/validator without inference: " + input);
                    }
                }
            }
            for (const auto& [verb, value] : std::vector<std::pair<std::string, int>>{
                     {"включи", 100}, {"включить", 100}, {"выключи", 0}, {"выключить", 0},
                     {"полностью включи", 100}, {"полностью выключи", 0}}) {
                for (const auto& input : {verb + " свет " + location, location + " " + verb + " свет"})
                    check(parser.parseChecked(input) == "L|" + std::string(1, code) + "|" + std::to_string(value), "on/off semantics unchanged");
            }
            for (const auto& [word, value] : std::vector<std::pair<std::string, int>>{{"наполовину", 50}, {"максимум", 100}}) {
                for (const auto& input : numericCommands(location, word))
                    check(parser.parseConfigured(input) == DeterministicParser::parse(input), "same accepted/rejected symbolic forms");
                const auto suffix = value == 100 ? "на максимум" : word;
                for (const auto& input : {"свет " + location + " " + suffix, location + " свет " + suffix,
                                          "установи свет " + location + " " + suffix})
                    check(parser.parseChecked(input) == "L|" + std::string(1, code) + "|" + std::to_string(value), "half/maximum unchanged");
            }
            for (const auto& bad : {"-1", "101", "00", "00050", "1.5", "40,5", "+40", "4e1", "40 или 50",
                                    "сорок", "примерно 40", "", "999999999999999999999", "40\nD"})
                for (const auto& input : numericCommands(location, bad))
                    check(!parser.parseChecked(input), "unsupported values remain unresolved: " + input);
            for (const auto& [other, otherCode] : locations) {
                (void)otherCode;
                for (const auto& input : {location + " и " + other + " свет 40", location + " свет 40 и " + other + " свет 50"})
                    check(!parser.parseChecked(input), "ambiguous/compound rooms remain unresolved");
            }
        }
        check(snapshot->mentionIndex().size() == 23, "complete mention domain retained");
        for (const auto& [alias, id] : snapshot->mentionIndex()) {
            check(domain.roomMention(alias) == domain.room(id), "every existing mention resolves canonically");
            // Bare mentions were never parser location phrases. Do not broaden
            // the parser to SafetyGate's inflections as part of this cutover.
            check(!parser.parseChecked(alias + " свет 40"), "bare room mention does not broaden grammar");
        }
        for (const auto& [input, expected] : std::vector<std::pair<std::string, std::string>>{
                 {" \tНа КуХнЕ поставь свет на 40 процентов!\r\n", "L|K|40"},
                 {"В СПАЛЬНЕ СВЕТ 40%", "L|B|40"}, {"В ГоСтИнОй свет 40!?", "L|G|40"},
                 {"СВЕТ В ЗАЛЕ НА МАКСИМУМ", "L|G|100"}, {"В КОРИДОРЕ ВЫКЛЮЧИ СВЕТ", "L|C|0"}})
            check(parser.parseChecked(input) == expected, "case/outside punctuation unchanged");
        for (const auto& input : {"на  кухне свет 40", "на\tкухне свет 40", "на кухне  свет 40", "на кухне, свет 40",
                 "!на кухне свет 40", "в ваной свет 40", "в ванной свет 40", "в детской свет 40", "свет в room_kitchen 40",
                 "свет 40", "на кухнейке свет 40", "на вокзале свет 40", "не включи свет на кухне", "открой входную дверь",
                 "открытвер на входе", "", " \n\t", "какая погода", "на кухне свет 40 и открой дверь"}) {
            check(!parser.parseChecked(input), "unsupported/normalization behavior unchanged");
            check(engine.process(input).compact() == "U", "unsupported command remains U with U fallback");
        }

        // A valid test-only alias reassignment must alter the NEW lookup. Keep
        // the checked production path rejecting this unapproved M4 difference.
        auto moved = snapshot->configuration();
        auto& living = room(moved, "room_living");
        auto& bedroom = room(moved, "room_bedroom");
        living.locations.erase(std::find(living.locations.begin(), living.locations.end(), "в зале"));
        bedroom.locations.push_back("в зале");
        for (const auto& alias : {"зал", "зале", "зала", "залу", "залом"}) {
            living.mentions.erase(std::find(living.mentions.begin(), living.mentions.end(), alias));
            bedroom.mentions.push_back(alias);
        }
        const runtime::DomainView reassigned(config::ConfigValidator::validate(moved));
        const DeterministicParser changed(reassigned);
        mismatch([&] { changed.requireLegacyRoomDomain(); });
        check(reassigned.roomLocation("в зале")->id == "room_bedroom", "valid config moved hall alias to bedroom");
        for (int value = 0; value <= 100; ++value) {
            const auto input = "в зале свет " + std::to_string(value);
            check(changed.parseConfigured(input) == "L|B|" + std::to_string(value), "config supplies new room result");
            check(DeterministicParser::parse(input) == "L|G|" + std::to_string(value), "legacy remains comparison only");
            mismatch([&] { changed.parseChecked(input); });
        }
        FakeLlm repair;
        repair.output = "L|G|40";
        CommandEngine protectedChanged(repair, changed);
        const auto rejected = protectedChanged.process("в зале свет 40");
        check(rejected.compact() == "U" && !rejected.error.empty() && repair.calls == 0, "mismatch is terminal; no legacy/LLM repair");

        auto removed = snapshot->configuration();
        auto& removedLiving = room(removed, "room_living");
        removedLiving.locations.erase(std::find(removedLiving.locations.begin(), removedLiving.locations.end(), "в зале"));
        const runtime::DomainView missingAlias(config::ConfigValidator::validate(removed));
        const DeterministicParser missingParser(missingAlias);
        check(!missingParser.parseConfigured("в зале свет 40"), "config removal really removes lookup");
        check(missingParser.parseConfigured("в гостиной свет 40") == "L|G|40", "unaffected alias still resolves");
        mismatch([&] { missingParser.requireLegacyRoomDomain(); });
        CommandEngine protectedMissing(repair, missingParser);
        check(protectedMissing.process("в зале свет 40").compact() == "U" && repair.calls == 0, "configured miss never falls back to legacy or LLM");

        auto added = snapshot->configuration();
        room(added, "room_kitchen").locations.push_back("в кухне");
        const runtime::DomainView additionalAlias(config::ConfigValidator::validate(added));
        const DeterministicParser additionalParser(additionalAlias);
        check(additionalParser.parseConfigured("в кухне свет 40") == "L|K|40", "new configured location observed in isolated parser");
        mismatch([&] { additionalParser.parseChecked("в кухне свет 40"); });

        auto restricted = snapshot->configuration();
        for (auto& action : restricted.actions) if (!action.parameters.empty()) action.parameters[0].maximum = 80;
        for (auto& device : restricted.devices) device.enabled = false;
        const runtime::DomainView restrictedDomain(config::ConfigValidator::validate(restricted));
        const DeterministicParser restrictedParser(restrictedDomain);
        restrictedParser.requireLegacyRoomDomain();
        check(restrictedParser.parseChecked("на кухне свет 100") == "L|K|100", "brightness/enablement have NOT cut over");

        Locations empty;
        const DeterministicParser emptyParser(empty);
        check(!emptyParser.parseConfigured("на кухне свет 40"), "required empty dependency has no implicit defaults");
        mismatch([&] { emptyParser.parseChecked("на кухне свет 40"); });
        Locations ambiguous;
        ambiguous.entries = {{"на кухне", 'K'}, {"на кухне", 'B'}};
        bool duplicateRejected = false;
        try { DeterministicParser invalid(ambiguous); } catch (const std::invalid_argument&) { duplicateRejected = true; }
        check(duplicateRejected, "ambiguous interface data rejected");
        auto invalid = snapshot->configuration();
        room(invalid, "room_bedroom").locations.push_back("на кухне");
        bool invalidRejected = false;
        try { config::ConfigValidator::validate(invalid); } catch (const config::ConfigError&) { invalidRejected = true; }
        check(invalidRejected, "invalid alias collision never reaches parser");
        check(parser.parseChecked("в зале свет 40") == "L|G|40", "other snapshots do not mutate running parser");

        std::ifstream dev(std::string(argv[1]) + "/tests/data/dev.tsv");
        check(bool(dev), "read frozen DEV cases");
        std::string line;
        int devCases = 0;
        while (std::getline(dev, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::size_t at = 0;
            for (int field = 0; field < 3; ++field) at = line.find('\t', at) + 1;
            check(parser.parseChecked(line.substr(at)) == DeterministicParser::parse(line.substr(at)), "frozen DEV parser equivalence");
            ++devCases;
        }
        check(devCases == 48, "full DEV set checked");
        std::cout << "ParserRoomCutover: " << checks << " checks passed; five locations, 23 mentions, full 0..100 grammar matrix; valid hall remap G->B observed and rejected by migration guard\n";
    } catch (const std::exception& error) {
        std::cerr << "ParserRoomCutover failed after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
