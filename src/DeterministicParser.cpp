#include "firefly/DeterministicParser.h"
#include "Text.h"
#include <array>
#include <map>
#include <regex>
#include <utility>
#include <vector>

namespace firefly {
namespace {
struct RoomRules {
    std::string location;
    std::string actionId, targetId, argumentId;
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
    std::map<std::string, std::string> unique;
    for (const auto& entry : locations) {
        if (entry.phrase.empty() || entry.actionId.empty() || entry.targetId.empty() || entry.argumentId.empty() || !unique.emplace(entry.phrase, entry.targetId).second)
            throw std::invalid_argument("Empty or duplicate parser location");
        RoomRules rule{entry.phrase, entry.actionId, entry.targetId, entry.argumentId, {}, {}};
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
actions::CandidateResult parseRules(std::string_view original, const std::vector<RoomRules>& rules) {
    // Normalize only the outside of the command; internal text stays intact.
    const auto first = original.find_first_not_of(" \t\r\n\f\v");
    if (first == std::string_view::npos) return actions::Unsupported{};
    const auto last = original.find_last_not_of(" \t\r\n\f\v.!?");
    if (last == std::string_view::npos || last < first) return actions::Unsupported{};
    const auto text = detail::lowercase(original.substr(first, last - first + 1));
    for (const auto& rule : rules) {
        const auto& location = rule.location;
        const auto candidate = [&rule](std::int64_t value) {
            return actions::GenericAction(rule.actionId, rule.targetId,
                {{rule.argumentId, actions::IntegerArgument(value)}});
        };
        std::smatch match;
        for (const auto& pattern : rule.numbers) {
            if (std::regex_match(text, match, pattern)) return candidate(std::stoi(match[1].str()));
        }
        for (const auto& pattern : rule.brightness) {
            if (std::regex_match(text, match, pattern)) {
                const auto value = match[1].str();
                if (value == "наполовину") return candidate(50);
                if (value == "максимум") return candidate(100);
                return candidate(std::stoi(match[2].str()));
            }
        }
        const std::array<std::pair<std::string, int>, 6> verbs{{
            {"включи", 100}, {"включить", 100}, {"выключи", 0},
            {"выключить", 0}, {"полностью включи", 100}, {"полностью выключи", 0}
        }};
        for (const auto& [verb, value] : verbs) {
            if (text == verb + " свет " + location || text == location + " " + verb + " свет")
                return candidate(value);
        }
        for (const auto& [suffix, value] : std::array<std::pair<std::string, int>, 2>{{
                 {"на максимум", 100}, {"наполовину", 50}}}) {
            if (text == "свет " + location + " " + suffix ||
                text == location + " свет " + suffix ||
                text == "установи свет " + location + " " + suffix)
                return candidate(value);
        }
    }
    return actions::Unsupported{};
}
}
struct DeterministicParser::Rules {
    const std::vector<ParserRoomLocation> locations;
    const std::vector<RoomRules> compiled;
    explicit Rules(std::vector<ParserRoomLocation> entries)
        : locations(std::move(entries)), compiled(compileRules(locations)) {}
};
DeterministicParser::DeterministicParser(const ParserRoomDomain& domain)
    : rules_(std::make_shared<const Rules>(domain.parserLocations())) {}
actions::CandidateResult DeterministicParser::parse(std::string_view original) const {
    return parseRules(original, rules_->compiled);
}
}
