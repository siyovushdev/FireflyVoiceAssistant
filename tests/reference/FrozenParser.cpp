#include "FrozenSemantics.h"
#include "Text.h"
#include <array>
#include <map>
#include <regex>
#include <utility>
#include <vector>

namespace firefly::reference {
namespace {
struct ParserRoomLocation { std::string phrase; char code; };
// LEGACY SHADOW ONLY. The original table stays here as the migration oracle.
// Production rules receive their location/code entries from ParserRoomDomain.
const std::vector<ParserRoomLocation>& legacyLocations() {
    static const std::vector<ParserRoomLocation> locations{
        {"на кухне", 'K'}, {"в спальне", 'B'}, {"в гостиной", 'G'},
        {"в зале", 'G'}, {"в коридоре", 'C'}};
    return locations;
}
struct RoomRules {
    std::string location;
    char room;
    std::vector<std::regex> numbers;
    std::vector<std::regex> brightness;
};
std::string regexLiteral(const std::string& phrase) {
    std::string result;
    for (const char character : phrase) {
        if (std::string_view(R"(\.^$|()[]{}*+?)").find(character) != std::string_view::npos)
            result += '\\';
        result += character;
    }
    return result;
}
std::vector<RoomRules> compileRules(const std::vector<ParserRoomLocation>& locations) {
    // Reuse the frozen whole-input productions; only their location data varies.
    std::vector<RoomRules> result;
    std::map<std::string, char> unique;
    for (const auto& entry : locations) {
        if (entry.phrase.empty() || !unique.emplace(entry.phrase, entry.code).second)
            throw std::invalid_argument("Empty or duplicate parser location");
        RoomRules rule{entry.phrase, entry.code, {}, {}};
        const auto location = regexLiteral(entry.phrase);
        const std::string number = "(0|[1-9][0-9]?|100)(%| процентов)?";
        for (const auto& pattern : {location + " свет " + number,
                 "свет " + location + " " + number, "установи свет " + location + " на " + number})
            rule.numbers.emplace_back(pattern);
        const std::string brightness = "(0|[1-9][0-9]?|100)(?:%| процентов)?|наполовину|максимум";
        for (const std::string verb : {"сделай", "поставь"}) {
            for (const auto& pattern : {
                     location + " " + verb + " свет (?:на )?(" + brightness + ")",
                     verb + " свет " + location + " на (" + brightness + ")"})
                rule.brightness.emplace_back(pattern);
        }
        rule.brightness.emplace_back("установи яркость света " + location + " (" + brightness + ")");
        // «на» precedes a numeric value or «максимум», not «наполовину».
        rule.brightness.emplace_back("установи яркость света " + location + " на (" + number + "|максимум)");
        result.push_back(std::move(rule));
    }
    return result;
}
std::optional<std::string> parseRules(std::string_view original, const std::vector<RoomRules>& rules) {
    // Normalize only the outside of the command; internal text stays intact.
    const auto first = original.find_first_not_of(" \t\r\n\f\v");
    if (first == std::string_view::npos) return std::nullopt;
    const auto last = original.find_last_not_of(" \t\r\n\f\v.!?");
    if (last == std::string_view::npos || last < first) return std::nullopt;
    const auto text = detail::lowercase(original.substr(first, last - first + 1));
    for (const auto& rule : rules) {
        const auto& location = rule.location;
        const auto code = [&rule](int value) {
            return std::string("L|") + rule.room + "|" + std::to_string(value);
        };
        std::smatch match;
        for (const auto& pattern : rule.numbers) {
            if (std::regex_match(text, match, pattern)) return code(std::stoi(match[1].str()));
        }
        for (const auto& pattern : rule.brightness) {
            if (std::regex_match(text, match, pattern)) {
                const auto value = match[1].str();
                if (value == "наполовину") return code(50);
                if (value == "максимум") return code(100);
                return code(std::stoi(match[2].str()));
            }
        }
        const std::array<std::pair<std::string, int>, 6> verbs{{
            {"включи", 100}, {"включить", 100}, {"выключи", 0},
            {"выключить", 0}, {"полностью включи", 100}, {"полностью выключи", 0}
        }};
        for (const auto& [verb, value] : verbs) {
            if (text == verb + " свет " + location || text == location + " " + verb + " свет")
                return code(value);
        }
        for (const auto& [suffix, value] : std::array<std::pair<std::string, int>, 2>{{
                 {"на максимум", 100}, {"наполовину", 50}}}) {
            if (text == "свет " + location + " " + suffix ||
                text == location + " свет " + suffix ||
                text == "установи свет " + location + " " + suffix)
                return code(value);
        }
    }
    return std::nullopt;
}
}
std::optional<std::string> FrozenParser::parse(std::string_view original) {
    static const auto rules = compileRules(legacyLocations());
    return parseRules(original, rules);
}
}
