#include "firefly/config/LanguageProfile.h"
#include "Platform.h"
#include <algorithm>
#include <sstream>
#include <tuple>
#include <codecvt>
#include <locale>
namespace firefly::config {
std::string normalizeLanguageAtom(std::string_view value) {
    auto folded = detail::lowercase(std::string(value));
    // Match DomainView token boundaries, so punctuation cannot hide collisions.
    const std::string separators = " \t\r\n.,!?;:()[]{}\"'";
    for (auto& character : folded) if (separators.find(character) != std::string::npos) character = ' ';
    std::istringstream input(folded);
    std::string word, result;
    while (input >> word) {
        if (!result.empty()) result += ' ';
        result += word;
    }
    return result;
}
void validateLanguageAtom(std::string_view value,bool action) {
    detail::checkText(value,"/language");
    if(value.empty()||value.size()>96)throw ConfigError("invalid_language","/language","Language atom must contain 1..96 UTF-8 bytes");
    for(unsigned char c:value)if(c<32||c==127||c=='<'||c=='>'||c=='{'||c=='}'||c=='['||c==']'||c=='|'||c=='`'||c=='$'||c==';'||c=='/'||c=='\\')throw ConfigError("unsafe_language","/language","Control or protocol syntax in language atom");
    std::istringstream in(normalizeLanguageAtom(value));std::string word;size_t count=0;bool meaningful=false;
    const std::set<std::string> reserved{"не","нет","ни","или","либо","игнорируй","инструкции","system","assistant","user","quit","exit","ignore","instructions","sudo","rm","execute","игнорировать","выполни"};
    while(in>>word){++count;meaningful=meaningful||(word!="в"&&word!="во"&&word!="на"&&word!="пожалуйста");if(reserved.count(word)||(!action&&(word=="включи"||word=="выключи"||word=="открой")))throw ConfigError("unsafe_language","/language","Reserved token in language atom");}
    if(!meaningful)throw ConfigError("unsafe_language","/language","Language atom cannot contain only neutral words");
    if(!count||count>8)throw ConfigError("invalid_language","/language","Language atom must contain 1..8 words");
}
std::vector<std::string> locationGrammar(const Room& room) {
    std::set<std::string> result;
    const auto ends = [](const std::string& s, const std::string& tail) {
        return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
    };
    // Only regular adjective paradigms. No stemming of arbitrary nouns, no
    // spelling repair, no expansion of numbers or action words.
    for (const auto* atoms : {&room.mentions, &room.locations, &room.metadata.aliases}) for (const auto& atom : *atoms) {
        std::istringstream input(normalizeLanguageAtom(atom));
        std::vector<std::string> words; std::string word;
        while (input >> word) words.push_back(word);
        if (!words.empty() && (words.front() == "в" || words.front() == "во" || words.front() == "на")) words.erase(words.begin());
        const bool roomPhrase = words.size() == 2 && words[1] == "комната";
        if (words.size() != 1 && !roomPhrase) continue;
        const auto& adjective = words[0];
        const bool soft = ends(adjective, "яя");
        const std::string ending = soft ? "яя" : ends(adjective, "ая") ? "ая" : ends(adjective, "ый") ? "ый" : "";
        if (ending.empty()) continue;
        const auto stem = adjective.substr(0, adjective.size() - ending.size());
        std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> utf8;
        const auto letters = utf8.from_bytes(stem);
        if (letters.size() < 3 || !std::all_of(letters.begin(), letters.end(), [](char32_t c) { return (c >= U'а' && c <= U'я') || c == U'ё'; })) continue;
        // Hushing/stressed paradigms need explicit reviewed forms. Do not
        // guess the stress needed for their endings or masculine nominative.
        if (!soft && (ends(stem,"ж") || ends(stem,"ш") || ends(stem,"ч") || ends(stem,"щ"))) continue;
        const bool spellingI = soft || ends(stem, "к") || ends(stem, "г") || ends(stem, "х");
        const std::vector<std::string> endings = soft
            ? std::vector<std::string>{"яя", "ей", "юю", "ее", "ем", "его", "ему", "им"}
            : std::vector<std::string>{"ая", "ой", "ую", "ое", "ом", "ого", "ому", spellingI ? "им" : "ым"};
        for (const auto& form : endings) result.insert(stem + form);
        if (roomPhrase) {
            result.insert(stem + (soft ? "яя" : "ая") + " комната");
            for (const auto* noun : {"комнаты", "комнате", "комнатой"}) result.insert(stem + (soft ? "ей " : "ой ") + noun);
            result.insert(stem + (soft ? "юю" : "ую") + " комнату");
        }
    }
    return {result.begin(), result.end()};
}
std::vector<LanguageConflict> languageConflicts(const Configuration& c) {
    std::vector<LanguageConflict> result;
    using Owners=std::map<std::string,std::set<std::string>>;
    Owners locations,devices,grammar;
    const auto add=[&](Owners& out,const std::vector<std::string>& values,const std::string& id){
        std::set<std::string> seen;
        for(const auto& value:values){const auto key=normalizeLanguageAtom(value);if(!seen.insert(key).second)result.push_back({"warning","duplicate_atom",key,{id}});out[key].insert(id);}
    };
    for(const auto& r:c.rooms){add(locations,r.mentions,r.id);add(locations,r.locations,r.id);add(locations,r.metadata.aliases,r.id);
        if(!r.metadata.container) for(const auto& atom:locationGrammar(r)) grammar[atom].insert(r.id);}
    for(const auto& d:c.devices){add(devices,d.spokenAliases,d.id);add(devices,d.metadata.aliases,d.id);}
    for(const auto& [alias,ids]:locations)if(ids.size()>1)result.push_back({"error","location_alias_conflict",alias,{ids.begin(),ids.end()}});
    for (auto& [alias, ids] : grammar) {
        const auto exact = locations.find(alias);
        if (exact != locations.end()) ids.insert(exact->second.begin(), exact->second.end());
        if (ids.size() > 1) result.push_back({"warning","location_grammar_ambiguity",alias,{ids.begin(),ids.end()}});
    }
    for(const auto& [alias,ids]:devices)if(ids.size()>1)result.push_back({"warning","contextual_device_alias",alias,{ids.begin(),ids.end()}});
    for (const auto& a : c.actions) for (const auto& parameter : a.parameters) {
        std::set<std::string> values;
        for (const auto& value : parameter.values) if (!values.insert(normalizeLanguageAtom(value)).second)
            result.push_back({"error", "value_interpretation_conflict", value, {a.id, parameter.id}});
    }
    for (auto a = devices.begin(); a != devices.end(); ++a) for (auto b = std::next(a); b != devices.end(); ++b)
        if (a->second != b->second && ((" " + a->first + " ").find(" " + b->first + " ") != std::string::npos ||
            (" " + b->first + " ").find(" " + a->first + " ") != std::string::npos))
            result.push_back({"warning", "overlapping_device_alias", a->first + " / " + b->first, {a->second.begin(), a->second.end()}});
    for(const auto& d:c.devices) {
        Owners actions;
        for(const auto& id:d.actionIds)for(const auto& a:c.actions)if(a.id==id)add(actions,a.aliases,id);
        for(const auto& [alias,ids]:actions)if(ids.size()>1)result.push_back({"error","action_alias_conflict",alias,{ids.begin(),ids.end()}});
        for(auto a=actions.begin();a!=actions.end();++a)for(auto b=std::next(a);b!=actions.end();++b)
            if(a->second!=b->second&&((" "+a->first+" ").find(" "+b->first+" ")!=std::string::npos||(" "+b->first+" ").find(" "+a->first+" ")!=std::string::npos))
                result.push_back({"warning","overlapping_action_alias",a->first+" / "+b->first,{d.id}});
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return std::tie(a.severity,a.code,a.alias,a.entities)<std::tie(b.severity,b.code,b.alias,b.entities);});
    return result;
}
}
