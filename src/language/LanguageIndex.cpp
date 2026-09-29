#include "firefly/language/LanguageIndex.h"
#include "firefly/language/Integer.h"
#include "../Text.h"
#include "../config/Platform.h"
#include <algorithm>
#include <regex>
#include <set>

namespace firefly::language {
const char* matchName(MatchKind kind) {
    switch (kind) {
        case MatchKind::ExactUnique: return "INDEX_EXACT";
        case MatchKind::FuzzyUnique: return "INDEX_FUZZY";
        case MatchKind::Ambiguous: return "INDEX_AMBIGUOUS";
        case MatchKind::Invalid: return "INDEX_INVALID";
        default: return "INDEX_MISS";
    }
}

LanguageIndex::LanguageIndex(config::Catalog catalog) : domain_(std::move(catalog)) {
    conflicts_ = config::languageConflicts(domain_.snapshot()->configuration());
    for (const auto& conflict : conflicts_) if (conflict.severity == "error")
        throw config::ConfigError(conflict.code, "/language", conflict.alias);
    std::set<std::pair<std::string, std::string>> unique;
    for (const auto& action : domain_.snapshot()->configuration().actions) for (const auto& alias : action.aliases) {
        config::validateLanguageAtom(alias, true);
        const auto normalized = config::normalizeLanguageAtom(alias);
        if (!unique.emplace(action.id, normalized).second) continue;
        auto words = detail::words(normalized);
        if (!words.empty()) actions_[words.front()].push_back({action.id, std::move(words)});
        bytes_ += sizeof(Atom) + action.id.size() + normalized.size();
    }
    fingerprint_ = config::detail::sha256(std::string(CompilerVersion) + ":" + catalogHash());
    bytes_ += sizeof(*this);
}

std::shared_ptr<const LanguageIndex> LanguageCompiler::compile(config::Catalog catalog) {
    return std::shared_ptr<const LanguageIndex>(new LanguageIndex(std::move(catalog)));
}

MatchResult LanguageIndex::match(std::string_view original) const {
    const auto reject = [](MatchKind kind, const std::string& reason) {
        return MatchResult{kind, actions::Unsupported{}, reason};
    };
    if (original.size() > 192 || original.find_first_not_of(" \t\r\n") == std::string_view::npos)
        return reject(MatchKind::Invalid, "Input length");
    try { config::detail::checkText(original, "/input"); }
    catch (const config::ConfigError&) { return reject(MatchKind::Invalid, "Invalid UTF-8"); }

    const auto& configuration = domain_.snapshot()->configuration();
    if (configuration.devices.empty()) return {};
    const auto grounding = domain_.resolve(original);
    if (grounding.ambiguous) return reject(MatchKind::Ambiguous, "Several configured entities match");
    if (grounding.devices.size() != 1) return {};
    const auto* target = domain_.device(grounding.devices.front());
    const auto intents = domain_.requestedActions(original, *target);
    if (intents.size() > 1) return reject(MatchKind::Ambiguous, "Several configured actions match");
    if (intents.empty()) return {};
    const auto& action = configuration.actions.at(domain_.snapshot()->actionIndex().at(intents.front()));
    if (action.parameters.size() > 1) return {};
    if (!action.parameters.empty() && action.parameters[0].type != "integer" &&
        action.parameters[0].type != "boolean" && action.parameters[0].type != "enum") return {};

    const auto words = detail::words(original);
    const auto lower = detail::lowercase(original);
    std::vector<bool> covered(words.size(), false);
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    std::size_t offset = 0;
    bool fuzzy = false;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const auto begin = lower.find(words[i], offset), end = begin + words[i].size();
        offset = end;
        spans.emplace_back(begin, end);
        for (const auto& mention : grounding.mentions) {
            const bool selected = (mention.category == "device" && mention.entityId == target->id) ||
                (mention.category == "location" && target->roomId && mention.entityId == *target->roomId);
            if (selected && begin >= mention.begin && end <= mention.end) {
                covered[i] = true;
                fuzzy = fuzzy || mention.distance > 0;
            }
        }
    }
    // Numeric identifiers inside a selected entity are not argument evidence.
    const auto entityCovered = covered;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const auto found = actions_.find(words[i]);
        if (found == actions_.end()) continue;
        for (const auto& atom : found->second)
            if (atom.action == action.id && i + atom.tokens.size() <= words.size() &&
                std::equal(atom.tokens.begin(), atom.tokens.end(), words.begin() + i))
                std::fill(covered.begin() + i, covered.begin() + i + atom.tokens.size(), true);
    }
    for (std::size_t i = 0; i < words.size(); ++i) if (!covered[i]) {
        if (words[i] == "и" || words[i] == "или" || words[i] == "либо")
            return reject(MatchKind::Ambiguous, "Coordinated or alternative command");
        if (words[i] == "не" || words[i] == "нет" || words[i] == "ни")
            return reject(MatchKind::Invalid, "Negated command");
    }

    if (action.parameters.empty()) for (std::size_t i = 0; i < words.size(); ++i)
        if (!covered[i] && (integerAt(words, i) || words[i] == "true" || words[i] == "false"))
            return reject(MatchKind::Invalid, "Unexpected argument for argument-free action");
    actions::GenericAction::Arguments arguments;
    if (!action.parameters.empty()) {
        const auto& parameter = action.parameters[0];
        std::vector<actions::Scalar> values;
        static const std::regex fractional("[0-9][.,][0-9]");
        if (parameter.type == "integer" && std::regex_search(original.begin(), original.end(), fractional))
            return reject(MatchKind::Invalid, "Fractional integer");
        for (std::size_t i = 0; i < words.size(); ++i) {
            if (entityCovered[i]) continue;
            if (parameter.type == "integer") {
                if (parameter.unit != "percent" && (percentWord(words[i]) || words[i].back() == '%'))
                    return reject(MatchKind::Invalid, "Percent value for a different unit");
                if (const auto number = integerAt(words, i)) {
                    if (i && words[i - 1] == "минус") return reject(MatchKind::Invalid, "Unresolved signed cardinal");
                    if (number->count == 2) {
                        const auto separator = original.substr(spans[i].second, spans[i + 1].first - spans[i].second);
                        if (entityCovered[i + 1] || separator.find_first_not_of(" \t\r\n") != std::string_view::npos)
                            return reject(MatchKind::Invalid, "Compound value crosses an entity or separator");
                    }
                    for (std::size_t j = 0; j < number->count; ++j) covered[i + j] = true;
                    values.emplace_back(number->value);
                    i += number->count - 1;
                }
            } else if (parameter.type == "boolean" && (words[i] == "true" || words[i] == "false")) {
                values.emplace_back(words[i] == "true");
                covered[i] = true;
            } else if (parameter.type == "enum" &&
                std::find(parameter.values.begin(), parameter.values.end(), words[i]) != parameter.values.end()) {
                values.emplace_back(words[i]);
                covered[i] = true;
            }
        }
        if (values.size() > 1) return reject(MatchKind::Invalid, "Multiple argument values");
        if (values.empty()) {
            for (std::size_t i = 0; i < words.size(); ++i)
                if (!covered[i] && (integerAt(words, i) || words[i] == "true" || words[i] == "false"))
                    return reject(MatchKind::Invalid, "Argument has a different scalar type");
            return {};
        }
        if (!config::accepts(parameter, values.front())) return reject(MatchKind::Invalid, "Argument outside schema");
        arguments.emplace(parameter.id, values.front());
        if (parameter.unit == "percent") for (std::size_t i = 0; i < words.size(); ++i)
            if (percentWord(words[i])) covered[i] = true;
    }
    for (std::size_t i = 0; i < words.size(); ++i) if (!covered[i]) {
        if (words[i] == "в" || words[i] == "во" || words[i] == "на" || words[i] == "пожалуйста") continue;
        return {}; // Unknown meaningful text must never be silently consumed.
    }
    return {fuzzy ? MatchKind::FuzzyUnique : MatchKind::ExactUnique,
        actions::GenericAction(action.id, target->id, std::move(arguments)), {}};
}
}
