#include "firefly/SafetyGate.h"
#include "Text.h"
#include <algorithm>
#include <regex>
#include <vector>

namespace firefly {
namespace {
bool lightWord(std::string_view word) {
    return word == "свет" || word == "света" || word == "освещение" ||
           word == "освещения" || word == "яркость" || word == "яркости";
}
bool onWord(std::string_view word) {
    return word == "включи" || word == "включите" || word == "включить" ||
           word == "включай" || word == "включайте";
}
bool offWord(std::string_view word) {
    return word == "выключи" || word == "выключите" || word == "выключить" ||
           word == "выключай" || word == "выключайте";
}
bool percentWord(std::string_view word) {
    return word == "%" || word == "процент" || word == "процента" || word == "процентов";
}
int cardinal(std::string_view word) {
    // A bounded safety vocabulary, not a parser route or transcript conversion.
    static constexpr std::string_view small[] = {"ноль", "один", "два", "три", "четыре",
        "пять", "шесть", "семь", "восемь", "девять", "десять", "одиннадцать",
        "двенадцать", "тринадцать", "четырнадцать", "пятнадцать", "шестнадцать",
        "семнадцать", "восемнадцать", "девятнадцать"};
    for (int value = 0; value < 20; ++value) if (word == small[value]) return value;
    static constexpr std::string_view tens[] = {"двадцать", "тридцать", "сорок", "пятьдесят",
        "шестьдесят", "семьдесят", "восемьдесят", "девяносто", "сто"};
    for (int index = 0; index < 9; ++index) if (word == tens[index]) return (index + 2) * 10;
    return word == "нуль" ? 0 : -1;
}
struct LightState {
    int minimum = 0;
    int maximum = 100;
    void require(int low, int high) {
        minimum = std::max(minimum, low);
        maximum = std::min(maximum, high);
    }
    bool impossible() const { return minimum > maximum; }
};
LightState requestedLightState(std::string_view original, const std::vector<std::string>& words) {
    LightState state;
    if (std::none_of(words.begin(), words.end(), lightWord)) return state;
    bool explicitState = false;
    for (const auto& word : words) {
        if (offWord(word)) { state.require(0, 0); explicitState = true; }
        if (onWord(word)) { state.require(1, 100); explicitState = true; }
        if (word == "максимум") { state.require(100, 100); explicitState = true; }
        if (word == "наполовину") { state.require(50, 50); explicitState = true; }
    }
    if (!explicitState) return state; // Numeric-only interpretation is not migrated.
    const auto reject = [] { return LightState{1, 0}; };
    // words() splits decimal punctuation. Never reinterpret a fractional state
    // requirement as separate integers (especially 0.0 as an OFF repair).
    static const std::regex fraction("[0-9][.,][0-9]");
    if (std::regex_search(original.begin(), original.end(), fraction)) return reject();
    bool percentageConsumed = false;
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (percentWord(words[i])) {
            if (!percentageConsumed) return reject(); // Unresolved explicit value.
            percentageConsumed = false;
            continue;
        }
        percentageConsumed = false;
        std::string_view token = words[i];
        bool attachedPercent = false;
        for (const auto suffix : {std::string_view("%"), std::string_view("процентов"),
                                 std::string_view("процента"), std::string_view("процент")}) {
            if (token.size() > suffix.size() && token.substr(token.size() - suffix.size()) == suffix) {
                token.remove_suffix(suffix.size());
                attachedPercent = true;
                break;
            }
        }
        int value = cardinal(token);
        const bool numeric = !token.empty() && ((token.front() >= '0' && token.front() <= '9') ||
                                                token.front() == '-' || token.front() == '+');
        if (numeric) {
            value = 0;
            for (const char digit : token) {
                if (digit < '0' || digit > '9' || value > 100) { value = -1; break; }
                value = value * 10 + digit - '0';
            }
        }
        std::size_t last = i;
        if (!numeric && !attachedPercent && value >= 20 && value <= 90 && i + 1 < words.size()) {
            const int unit = cardinal(words[i + 1]);
            if (unit > 0 && unit < 10) { value += unit; ++last; }
        }
        const bool unitFollows = last + 1 < words.size() && percentWord(words[last + 1]);
        const auto previous = i == 0 ? std::string_view{} : std::string_view(words[i - 1]);
        // Percentages are explicit. A bare value needs the existing brightness
        // slot ("на N", light/state word, or terminal value), not an unrelated unit.
        const bool bare = (numeric || value >= 0) &&
            (last + 1 == words.size() || previous == "на" || lightWord(previous) ||
             previous == "максимум" || previous == "наполовину");
        if (!attachedPercent && !unitFollows && !bare) continue;
        if (value < 0 || value > 100 || previous == "минус") return reject();
        state.require(value, value);
        percentageConsumed = unitFollows;
        i = last;
    }
    return state;
}
}
actions::CandidateResult SafetyGate::apply(std::string_view original, const actions::CandidateResult& candidate,
                                           const runtime::DomainView& domain) {
    const auto* action = std::get_if<actions::GenericAction>(&candidate);
    if (!action) return actions::Unsupported{};
    const auto description = domain.describe(*action);
    if (!description) return actions::Unsupported{};
    const auto words = detail::words(original);
    const auto state = requestedLightState(original, words);
    // Contradictory ORIGINAL requirements reject every candidate, including a
    // guessed OFF value. This shared boundary never chooses an interpretation.
    if (state.impossible()) return actions::Unsupported{};
    if (description->definition->operation == config::Operation::OpenEntranceDoor) {
        const auto text = detail::lowercase(original);
        const bool openIntent = std::any_of(words.begin(), words.end(), [](const auto& word) {
            return word == "открой" || word == "откройте" || word == "открыть" ||
                   word == "открывай" || word == "открывайте";
        });
        bool entranceReference = text.find("входн") != std::string::npos;
        for (std::size_t i = 1; i < words.size(); ++i) {
            if (words[i - 1] == "на" && words[i] == "входе") entranceReference = true;
        }
        // Require all three explicit cues in the original input. Never repair STT.
        if (!openIntent || !entranceReference ||
            text.find("двер") == std::string::npos) return actions::Unsupported{};
        return candidate;
    }
    const auto brightness = *description->brightness;
    if (brightness < state.minimum || brightness > state.maximum) return actions::Unsupported{};
    // Ground the target through snapshot aliases. Multiple different room
    // identities are ambiguous even if one happens to match the candidate.
    const config::Room* mentioned = nullptr;
    for (const auto& word : words) {
        const auto* room = domain.roomMention(word);
        if (!room) continue;
        if (mentioned && mentioned->id != room->id) return actions::Unsupported{};
        mentioned = room;
    }
    if (!mentioned || !description->target->roomId || mentioned->id != *description->target->roomId)
        return actions::Unsupported{};
    // Conservative stem detection also covers inflections and mixed case.
    if (brightness == 0 && detail::lowercase(original).find("включ") != std::string::npos)
        return actions::Unsupported{};
    return candidate;
}
}
