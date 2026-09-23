#include "firefly/runtime/DomainView.h"
#include "../Text.h"
#include <algorithm>
#include <codecvt>
#include <locale>
#include <set>
#include <charconv>
#include <cmath>
#include <cerrno>
#include <cstdlib>

namespace firefly::runtime {
namespace {
struct Token { std::string text; std::size_t begin, end; };
std::vector<Token> tokens(std::string_view original) {
    const auto lower = detail::lowercase(original);
    const std::string separators = " \t\r\n.,!?;:()[]{}\"'";
    std::vector<Token> result;
    for (auto begin = lower.find_first_not_of(separators); begin != std::string::npos;) {
        auto end = lower.find_first_of(separators, begin);
        if (end == std::string::npos) end = lower.size();
        result.push_back({lower.substr(begin, end - begin), begin, end});
        begin = lower.find_first_not_of(separators, end);
    }
    return result;
}
bool digit(const std::string& text) { return text.find_first_of("0123456789") != std::string::npos; }
bool oneEdit(const std::string& a, const std::string& b) {
    std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> utf8;
    const auto x = utf8.from_bytes(a), y = utf8.from_bytes(b);
    // Only long alphabetic words, at most one insertion/deletion/substitution.
    // No transposition shortcut, short-word correction, numbers or intent words.
    if (x.size() < 6 || y.size() < 6 || x.size() + 1 < y.size() || y.size() + 1 < x.size()) return false;
    static const std::set<std::string> protectedWords{"включи","выключи","открой","открыть","закрой","закрыть","вперед","вперёд","назад","процентов","процента","процент","отключи","запусти","останови","минус"};
    if (protectedWords.count(a) || protectedWords.count(b)) return false;
    const auto numericWord=[](const std::string& word){
        for(const auto* stem:{"одиннадц","двенадц","тринадц","четырнадц","пятнадц","шестнадц","семнадц","восемнадц","девятнадц","двадц","тридц","пятьдесят","шестьдесят","семьдесят","восемьдесят","девяност","перв","втор","трет","четверт","четвёрт","пят","шест","седьм","восьм","девят","десят"})
            if(word.rfind(stem,0)==0)return true;
        return false;
    };
    if(numericWord(a)||numericWord(b))return false;
    const auto letters = [](const auto& word) { return std::all_of(word.begin(), word.end(), [](char32_t c) {
        return (c >= U'a' && c <= U'z') || (c >= U'а' && c <= U'я') || c == U'ё';
    }); };
    if (!letters(x) || !letters(y)) return false;
    std::size_t i = 0, j = 0; int edits = 0;
    while (i < x.size() && j < y.size()) {
        if (x[i] == y[j]) { ++i; ++j; continue; }
        if (++edits > 1) return false;
        if (x.size() >= y.size()) ++i;
        if (y.size() >= x.size()) ++j;
    }
    return edits + (i < x.size() || j < y.size() ? 1 : 0) == 1;
}
void collect(std::vector<MentionEvidence>& matches, const std::vector<Token>& input, std::string_view original,
             const std::string& id, const std::string& category, const std::string& alias, bool fuzzy, bool numbered = false) {
    const auto words = tokens(alias);
    if (words.empty() || words.size() > input.size()) return;
    for (std::size_t start = 0; start + words.size() <= input.size(); ++start) {
        int distance = 0; bool valid = true;
        bool numeric = false;
        for (std::size_t i = 0; i < words.size(); ++i) numeric = numeric || digit(words[i].text) || digit(input[start + i].text);
        for (std::size_t i = 0; i < words.size(); ++i) {
            if (words[i].text == input[start + i].text) continue;
            if (!fuzzy || numeric || !oneEdit(words[i].text, input[start + i].text) || ++distance > 1) { valid = false; break; }
        }
        if (!valid) continue;
        // Never match a partial numbered identifier: "цех" is not evidence for
        // "цех 11"; a configured numbered phrase must match its number exactly.
        const auto last = start + words.size();
        if (numbered && !numeric && last < input.size() && digit(input[last].text)) continue;
        const auto begin = input[start].begin, end = input[last - 1].end;
        matches.push_back({id, category, alias, std::string(original.substr(begin, end - begin)), distance ? "edit_distance_1" : "exact", begin, end, distance});
    }
}
void preferSpecific(std::vector<MentionEvidence>& matches) {
    std::vector<MentionEvidence> selected;
    for (const auto& m : matches) {
        const bool dominated = std::any_of(matches.begin(), matches.end(), [&](const auto& other) {
            if (m.category != other.category || other.begin > m.begin || other.end < m.end) return false;
            return other.distance < m.distance || (other.distance == m.distance && other.end - other.begin > m.end - m.begin);
        });
        if (!dominated && std::none_of(selected.begin(), selected.end(), [&](const auto& x) {
            return x.entityId == m.entityId && x.begin == m.begin && x.end == m.end && x.distance == m.distance;
        })) selected.push_back(m);
    }
    matches = std::move(selected);
}
}
Grounding DomainView::resolve(std::string_view original, bool allowTypo) const {
    Grounding result; result.catalogHash = snapshot_->contentHash();
    const auto input = tokens(original);
    for (const auto& location : snapshot_->configuration().rooms) {
        if (location.metadata.container) continue;
        const bool numbered = std::any_of(location.mentions.begin(), location.mentions.end(), digit) || std::any_of(location.locations.begin(), location.locations.end(), digit) ||
            std::any_of(location.metadata.aliases.begin(),location.metadata.aliases.end(),digit);
        for (const auto& alias : location.mentions) collect(result.mentions, input, original, location.id, "location", alias, allowTypo, numbered);
        for (const auto& alias : location.locations) collect(result.mentions, input, original, location.id, "location", alias, allowTypo, numbered);
        if (snapshot_->configuration().schemaVersion == 2)
            for (const auto& alias : location.metadata.aliases) collect(result.mentions,input,original,location.id,"location",alias,allowTypo,numbered);
    }
    if (snapshot_->configuration().schemaVersion == 2) for (const auto& device : snapshot_->configuration().devices) {
        const bool fuzzy = allowTypo && device.kind != config::DeviceKind::EntranceDoor;
        const bool numbered = std::any_of(device.spokenAliases.begin(), device.spokenAliases.end(), digit) || std::any_of(device.metadata.aliases.begin(), device.metadata.aliases.end(), digit);
        for (const auto& alias : device.spokenAliases) collect(result.mentions, input, original, device.id, "device", alias, fuzzy, numbered);
        for (const auto& alias : device.metadata.aliases) collect(result.mentions, input, original, device.id, "device", alias, fuzzy, numbered);
    }
    preferSpecific(result.mentions);
    std::set<std::string> locations, devices;
    for (const auto& m : result.mentions) if (m.category == "location") locations.insert(m.entityId);
    for (const auto& m : result.mentions) if (m.category == "device") {
        const auto* target = device(m.entityId);
        if (locations.empty() || (target->roomId && locations.count(*target->roomId))) devices.insert(m.entityId);
    }
    result.locations.assign(locations.begin(), locations.end()); result.devices.assign(devices.begin(), devices.end());
    result.ambiguous = locations.size() > 1 || devices.size() > 1;
    if (locations.size() == 1) for (const auto& mention : result.mentions) if (mention.category == "device") {
        const bool compatibleSpan = std::any_of(result.mentions.begin(), result.mentions.end(), [&](const auto& other) {
            const auto* target = device(other.entityId);
            return other.category == "device" && other.begin == mention.begin && other.end == mention.end &&
                target && target->roomId && locations.count(*target->roomId);
        });
        if (!compatibleSpan) result.ambiguous = true;
    }
    return result;
}
std::vector<std::string> DomainView::requestedActions(std::string_view original, const config::Device& target) const {
    const auto input = tokens(original);
    std::vector<MentionEvidence> matches;
    for (const auto& id : target.actionIds) {
        const auto& action = snapshot_->configuration().actions.at(snapshot_->actionIndex().at(id));
        for (const auto& alias : action.aliases) collect(matches, input, original, id, "action", alias, false);
    }
    preferSpecific(matches);
    std::set<std::string> ids;
    for (const auto& m : matches) ids.insert(m.entityId);
    return {ids.begin(), ids.end()};
}
actions::CandidateResult DomainView::parseDeclared(std::string_view original) const {
    if (snapshot_->configuration().schemaVersion != 2) return actions::Unsupported{};
    const auto grounding = resolve(original);
    if (grounding.ambiguous || grounding.devices.size() != 1) return actions::Unsupported{};
    const auto* target = device(grounding.devices.front());
    if (!target || target->kind != config::DeviceKind::Generic) return actions::Unsupported{};
    const auto intents = requestedActions(original,*target);
    if (intents.size() != 1) return actions::Unsupported{};
    const auto& definition = snapshot_->configuration().actions.at(snapshot_->actionIndex().at(intents.front()));
    if (definition.operation != config::Operation::AssignState || definition.parameters.size() > 1) return actions::Unsupported{};
    actions::GenericAction::Arguments arguments;
    const auto input = tokens(original);
    if (!definition.parameters.empty()) {
        const auto& p = definition.parameters.front();
        std::vector<actions::Scalar> values;
        for (const auto& token : input) {
            const bool entitySpan = std::any_of(grounding.mentions.begin(),grounding.mentions.end(),[&](const auto& m){return token.begin>=m.begin&&token.end<=m.end;});
            if (entitySpan) continue;
            if (p.type == "integer") {
                std::int64_t number = 0;const auto parsed = std::from_chars(token.text.data(),token.text.data()+token.text.size(),number);
                if (parsed.ec == std::errc{} && parsed.ptr == token.text.data()+token.text.size()) values.emplace_back(number);
            } else if (p.type == "boolean" && (token.text == "true" || token.text == "false")) values.emplace_back(token.text == "true");
            else if (p.type == "enum" && std::find(p.values.begin(),p.values.end(),token.text) != p.values.end()) values.emplace_back(token.text);
        }
        // Decimal numbers and strings deliberately use structured model ingress;
        // do not reconstruct punctuation or silently pick one of several values.
        if (p.type == "number" || p.type == "string" || values.size() != 1) return actions::Unsupported{};
        arguments.emplace(p.id,values.front());
    }
    std::vector<MentionEvidence> intentSpans;
    for(const auto& alias:definition.aliases)collect(intentSpans,input,original,definition.id,"action",alias,false);
    for(const auto& token:input) {
        const auto covered=[&](const auto& spans){return std::any_of(spans.begin(),spans.end(),[&](const auto& m){return token.begin>=m.begin&&token.end<=m.end;});};
        if(covered(grounding.mentions)||covered(intentSpans))continue;
        if(token.text=="в"||token.text=="во"||token.text=="на"||token.text=="пожалуйста")continue;
        bool argument=false;
        for(const auto& [id,value]:arguments){(void)id;argument=argument||std::visit([&](const auto& v){using T=std::decay_t<decltype(v)>;if constexpr(std::is_same_v<T,std::string>)return token.text==v;else if constexpr(std::is_same_v<T,bool>)return token.text==(v?"true":"false");else return token.text==std::to_string(v);},value.data());}
        if(argument)continue;
        if(!definition.parameters.empty()&&definition.parameters.front().unit=="percent"&&(token.text=="процентов"||token.text=="процента"||token.text=="процент"||token.text=="%"))continue;
        return actions::Unsupported{};
    }
    return actions::GenericAction(definition.id,target->id,std::move(arguments));
}

}
