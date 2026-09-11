#include "firefly/DeterministicParser.h"
#include "Text.h"
#include <array>
#include <regex>

namespace firefly {
std::optional<std::string> DeterministicParser::parse(std::string_view original) {
    // Normalize only the outside of the command; internal text stays intact.
    const auto first = original.find_first_not_of(" \t\r\n\f\v");
    if (first == std::string_view::npos) return std::nullopt;
    const auto last = original.find_last_not_of(" \t\r\n\f\v.!?");
    if (last == std::string_view::npos || last < first) return std::nullopt;
    const auto text = detail::lowercase(original.substr(first, last - first + 1));
    // Entire-input grammar: unknown words, negation and compound commands fall through.
    const std::array<std::pair<std::string, char>, 5> locations{{
        {"на кухне", 'K'}, {"в спальне", 'B'}, {"в гостиной", 'G'},
        {"в зале", 'G'}, {"в коридоре", 'C'}
    }};
    for (const auto& [location, room] : locations) {
        const auto code = [room](int value) {
            return std::string("L|") + room + "|" + std::to_string(value);
        };
        std::smatch match;
        const std::string number = "(0|[1-9][0-9]?|100)(%| процентов)?";
        for (const auto& pattern : {location + " свет " + number,
                                    "свет " + location + " " + number,
                                    "установи свет " + location + " на " + number}) {
            if (std::regex_match(text, match, std::regex(pattern)))
                return code(std::stoi(match[1].str()));
        }
        // Narrow imperative forms only: one explicit location and one absolute value.
        const std::string brightness = "(0|[1-9][0-9]?|100)(?:%| процентов)?|наполовину|максимум";
        for (const std::string verb : {"сделай", "поставь"}) {
            for (const auto& pattern : {
                     location + " " + verb + " свет (?:на )?(" + brightness + ")",
                     verb + " свет " + location + " на (" + brightness + ")"}) {
                if (std::regex_match(text, match, std::regex(pattern))) {
                    const auto value = match[1].str();
                    if (value == "наполовину") return code(50);
                    if (value == "максимум") return code(100);
                    return code(std::stoi(match[2].str()));
                }
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
