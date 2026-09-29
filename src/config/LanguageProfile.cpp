#include "firefly/config/LanguageProfile.h"
#include "Platform.h"
#include <algorithm>
#include <sstream>
#include <tuple>
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
std::vector<LanguageConflict> languageConflicts(const Configuration& c) {
    std::vector<LanguageConflict> result;
    using Owners=std::map<std::string,std::set<std::string>>;
    Owners locations,devices;
    const auto add=[&](Owners& out,const std::vector<std::string>& values,const std::string& id){
        std::set<std::string> seen;
        for(const auto& value:values){const auto key=normalizeLanguageAtom(value);if(!seen.insert(key).second)result.push_back({"warning","duplicate_atom",key,{id}});out[key].insert(id);}
    };
    for(const auto& r:c.rooms){add(locations,r.mentions,r.id);add(locations,r.locations,r.id);add(locations,r.metadata.aliases,r.id);}
    for(const auto& d:c.devices){add(devices,d.spokenAliases,d.id);add(devices,d.metadata.aliases,d.id);}
    for(const auto& [alias,ids]:locations)if(ids.size()>1)result.push_back({"error","location_alias_conflict",alias,{ids.begin(),ids.end()}});
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
