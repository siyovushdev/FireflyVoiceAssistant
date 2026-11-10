#pragma once
#include "DialogueOperations.h"
#include "firefly/admin/AdminCopilot.h"
#include "firefly/admin/AdminRequestPolicy.h"
#include "firefly/runtime/DomainView.h"
#include "../Text.h"
#include "AdminSetupSource.h"
#include <regex>

namespace firefly::admin {
// Runs before private draft persistence as well as before any inference.
inline void validateSetupDocumentInput(std::string_view text) {
    validateMessage(text);
    // Permit an explicit decision to enter credentials in their separate form.
    // Only complete, value-free clauses are removed; actual secret assignments
    // remain rejected before inference, history or checkpoint persistence.
    const auto folded = std::regex_replace(detail::lowercase(text),
        std::regex(R"((без пароля|без паролей|пароль введу отдельно|пароли введу отдельно|пароли задаются отдельно в настройках доступа|не придумывай (?:логин и пароль|пароль и логин|пароль|пароли)|no password|without password)([ \t]*[.,;!?]|$))"), "$2");
    if (folded.find("парол") != std::string::npos || folded.find("password") != std::string::npos ||
        folded.find("passwd") != std::string::npos ||
        std::regex_search(folded, std::regex(R"((secret|token|username|login|логин)["' \t]*[:=])")))
        throw std::invalid_argument("admin_private_credentials: Пароли и логины задайте отдельно; в запросе используйте credentials_id.");
}
namespace setup_document {
inline bool literal(std::string_view input, const std::string& value) {
    if (value.empty()) return true;
    std::size_t at = 0;
    const auto word = [](unsigned char c) { return c >= 128 || std::isalnum(c) || c == '_'; };
    while ((at = input.find(value, at)) != std::string_view::npos) {
        bool left = at == 0 || !word(static_cast<unsigned char>(value.front())) || !word(static_cast<unsigned char>(input[at - 1]));
        const auto end = at + value.size();
        bool right = end == input.size() || !word(static_cast<unsigned char>(value.back())) || !word(static_cast<unsigned char>(input[end]));
        for(const std::string quote:{"«","“","„"})
            if(at>=quote.size()&&input.substr(at-quote.size(),quote.size())==quote)left=true;
        for(const std::string quote:{"»","”","“"})
            if(input.substr(end,quote.size())==quote)right=true;
        if (left && right) return true;
        ++at;
    }
    return false;
}
inline bool named(std::string_view input, const std::string& name) {
    return literal(detail::lowercase(input), detail::lowercase(name));
}
inline std::string stem(std::string word) {
    // Inflection only; never fuzzy spelling correction or synonym substitution.
    for (const auto* suffix : {"иями","ями","ами","ого","ему","ому","ыми","ими","иях","ах","ях","ой","ей","ий","ый","ая","яя","ое","ее","ом","ем","ам","ям","ов","ев","ы","и","а","я","у","ю","е","о"}) {
        const std::string ending(suffix);
        if (word.size()>ending.size()+3 && word.compare(word.size()-ending.size(),ending.size(),ending)==0) { word.resize(word.size()-ending.size());break; }
    }
    return word;
}
inline bool mentioned(std::string_view input,const std::string& name) {
    auto words=detail::words(detail::lowercase(input));auto phrase=detail::words(detail::lowercase(name));
    for(auto& word:words)word=stem(std::move(word));
    for(auto& word:phrase)word=stem(std::move(word));
    return !phrase.empty()&&std::search(words.begin(),words.end(),phrase.begin(),phrase.end())!=words.end();
}
inline std::set<std::string> mentionedTargets(std::string_view input,const std::vector<std::string>& names) {
    auto words=detail::words(detail::lowercase(input));for(auto& word:words)word=stem(std::move(word));
    struct Span {std::size_t begin,end;std::string name;};std::vector<Span> spans;
    for(const auto& name:names) {
        auto phrase=detail::words(detail::lowercase(name));for(auto& word:phrase)word=stem(std::move(word));
        if(phrase.empty())continue;
        for(std::size_t i=0;i+phrase.size()<=words.size();++i)
            if(std::equal(phrase.begin(),phrase.end(),words.begin()+i))spans.push_back({i,i+phrase.size(),name});
    }
    std::set<std::string> targets;
    for(const auto& span:spans) {
        // «Рабочая лампа» names one device, not also the existing «Лампа».
        // A separate occurrence in «Лампа и Рабочая лампа» still names both.
        const bool covered=std::any_of(spans.begin(),spans.end(),[&](const Span& other){
            return other.begin<=span.begin&&other.end>=span.end&&other.end-other.begin>span.end-span.begin;
        });
        if(!covered)targets.insert(span.name);
    }
    return targets;
}
inline std::vector<std::string> clauses(std::string_view input) {
    const auto text=detail::lowercase(input);std::vector<std::string> out;std::size_t begin=0;
    for(std::size_t i=0;i<=text.size();++i) if(i==text.size()||text[i]==';'||text[i]=='\n'||text[i]=='!'||text[i]=='?'||
        (text[i]=='.' && !(i>0&&i+1<text.size()&&std::isdigit(static_cast<unsigned char>(text[i-1]))&&std::isdigit(static_cast<unsigned char>(text[i+1]))))) {
        out.push_back(text.substr(begin,i-begin));begin=i+1;
    }
    return out;
}
inline bool powerPair(std::string_view text) {
    return (text.find("включ")!=std::string::npos&&(text.find("выключ")!=std::string::npos||text.find("отключ")!=std::string::npos))||text.find("on/off")!=std::string::npos;
}
inline bool powerFor(std::string_view input,const std::string& name,const std::vector<std::string>& names) {
    bool positive=false,negative=false,on=false,off=false;std::set<std::string> previousTargets;
    for(const auto& clause:clauses(input)) {
        const auto targets=mentionedTargets(clause,names);
        const bool continuation=targets.empty()&&previousTargets.count(name)&&
            (named(clause,"только")||named(clause,"он")||named(clause,"она")||named(clause,"оно")||named(clause,"они")||named(clause,"его")||named(clause,"их"));
        if(!targets.empty())previousTargets=targets;
        const bool collective=named(clause,"оба")||named(clause,"все")||named(clause,"всех")||named(clause,"всем")||named(clause,"обоих")||named(clause,"обоим")||named(clause,"каждое")||clause.find("эти устройства")!=std::string::npos;
        // A device specification commonly puts ON and OFF on separate list
        // lines. Keep those lines scoped to the last explicitly named device;
        // a new named device ends the scope. Never borrow another device's pair.
        const bool listLine=targets.empty()&&previousTargets.size()==1&&previousTargets.count(name)&&
            (clause.find("- включение")!=std::string::npos||clause.find("- выключение")!=std::string::npos);
        if(!mentioned(clause,name)&&!collective&&!continuation&&!listLine)continue;
        negative=negative||clause.find("без управлен")!=std::string::npos||clause.find("без действ")!=std::string::npos||clause.find("не управл")!=std::string::npos||clause.find("не включ")!=std::string::npos||clause.find("не выключ")!=std::string::npos;
        positive=positive||powerPair(clause);
        if(listLine) {on=on||clause.find("- включение")!=std::string::npos;off=off||clause.find("- выключение")!=std::string::npos;}
    }
    return (positive||(on&&off))&&!negative;
}
inline bool targetMentioned(std::string_view input,const Json& catalog,const Json& focus,const Json& ref,const char* category) {
    if(!ref.is_string())return false;
    const auto name=ref.get<std::string>();if(mentioned(input,name))return true;
    std::vector<const Json*> selected;
    for(const auto& entity:catalog.at(category))if(std::find(focus.begin(),focus.end(),entity.at("id"))!=focus.end())selected.push_back(&entity);
    const auto folded=detail::lowercase(input);const auto words=detail::words(folded);
    const bool pronoun=std::find(words.begin(),words.end(),"его")!=words.end()||std::find(words.begin(),words.end(),"её")!=words.end()||std::find(words.begin(),words.end(),"ее")!=words.end()||std::find(words.begin(),words.end(),"ему")!=words.end();
    return pronoun&&selected.size()==1&&(selected[0]->at("id")==ref||selected[0]->at("name")==ref);
}
inline void fact(const Json& value, const Json& old, std::string_view input, const std::string& key) {
    if (value == old) return;
    if(value.is_boolean()&&(key=="mqtt.tls"||key=="mqtt.retain")) {
        const auto field=key.substr(key.find_last_of('.')+1);const auto text=detail::lowercase(input);
        if(!std::regex_search(text,std::regex("(?:^|[^a-z0-9_])"+field+"[ \\t\"':=]+"+value.dump()+"(?:$|[^a-z0-9_])")))
            throw std::invalid_argument("unstated_connection_fact: explicit "+field+" change required");
    }
    if(value.is_number_integer()) {
        const auto field=key.substr(key.find_last_of('.')==std::string::npos?0:key.find_last_of('.')+1);
        const std::map<std::string,std::string> labels={{"address","address|адрес регистра|адрес катушки|адрес coil"},{"unit_id","unit_id|unit id|slave_id"},
            {"port","port|порт"},{"function","function|fc|функцию|функция|функции"},{"baud_rate","baud_rate|baud|бод"},{"stop_bits","stop_bits"}};
        const auto label=labels.find(field);
        if(label!=labels.end()) {
            const auto text=detail::lowercase(input);const std::regex pattern("(?:^|[^a-z0-9_])(?:"+label->second+")[ \\t\"':=]*([0-9]+)");
            bool present=false,matched=false;
            for(auto it=std::sregex_iterator(text.begin(),text.end(),pattern);it!=std::sregex_iterator();++it) {present=true;matched=matched||it->str(1)==value.dump();}
            if(present&&!matched)throw std::invalid_argument("unstated_connection_fact: "+key+" differs from the explicitly labelled value");
        }
    }
    if (value.is_string() && (literal(input, value.get<std::string>())||setup_source::quotedLiteral(input,value.get<std::string>()))) return;
    if ((value.is_number() || value.is_boolean()) && literal(input, value.dump())) return;
    throw std::invalid_argument("unstated_connection_fact: " + key + "; укажите точное значение в запросе");
}
inline void facts(const Json& value, const Json& old, std::string_view input, const std::string& path = "") {
    if (!value.is_object()) { fact(value, old, input, path); return; }
    for (const auto& item : value.items()) {
        const auto& key = item.key(); const auto& v = item.value();
        const Json prior = old.is_object() && old.contains(key) ? old.at(key) : Json();
        if (key == "password" || key == "username" || key == "secret" || key == "token")
            throw std::invalid_argument("admin_private_credentials");
        // Public application defaults, not claims about the endpoint or device.
        if ((prior.is_null() || v == prior) && ((key == "timeout_ms" && v == 2000) || (key == "qos" && v == 0) ||
            (key == "retain" && v == false) || (key == "tls" && v == false) ||
            (key == "base_path" && v == "") || (key == "expected_status" && v == 200))) continue;
        // These are protocol types, not invented device facts. Text is the
        // only MQTT observation format emitted by the typed planner; Modbus
        // coil functions carry booleans by definition.
        if(key=="format"&&v=="text"&&path=="mqtt.")continue;
        if(key=="data_type"&&v=="boolean"&&path=="modbus."&&value.contains("function")&&
           (value.at("function")==1||value.at("function")==2||value.at("function")==5))continue;
        if(key=="base_url"&&v.is_string()&&path=="http."&&!literal(input,v.get<std::string>())) {
            std::smatch match;const auto url=v.get<std::string>();
            if(std::regex_match(url,match,std::regex(R"(http://([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+):([0-9]+))"))&&
               named(input,"HTTP")&&literal(input,match.str(1))) {
                fact(std::stoi(match.str(2)),Json(),input,"port");continue;
            }
        }
        if (key == "type") {
            if (!v.is_string() || (!named(input, v.get<std::string>()) && v != prior &&
                !(v == "modbus_tcp" && named(input,"Modbus") && named(input,"TCP")) &&
                !(v == "modbus_rtu" && named(input,"Modbus") && named(input,"RTU"))))
                throw std::invalid_argument("unstated_connection_transport");
            continue;
        }
        if (key == "fields") {
            if (!v.is_object()) throw std::invalid_argument("Observation fields must be an object");
            for (const auto& field : v.items()) {
                if (!literal(input, field.key()) && (!prior.is_object() || !prior.contains(field.key())))
                    throw std::invalid_argument("unstated_observation_response_field");
                // State references are checked by ConfigLoader, not invented facts.
                if (!field.value().is_string()) throw std::invalid_argument("Invalid state field reference");
            }
            continue;
        }
        if (key == "field" || key == "argument") {
            if (!v.is_string()) throw std::invalid_argument("Invalid declared-field reference");
            continue; // Authoritative schema checks the selected device/action.
        }
        if (key == "body") {
            if (!v.is_object()) throw std::invalid_argument("HTTP body must be an object");
            for (const auto& field : v.items()) {
                if (!literal(input, field.key()) && (!prior.is_object() || !prior.contains(field.key())))
                    throw std::invalid_argument("unstated_http_body_field");
                fact(field.value(), prior.is_object() ? prior.value(field.key(),Json()) : Json(), input, field.key());
            }
            continue;
        }
        if (v.is_object()) facts(v, prior, input, path + key + ".");
        else fact(v, prior, input, path + key);
    }
}
inline void requireExplicitCoverage(const Json& candidate,std::string_view input) {
    std::string text;
    for(const auto& clause:setup_source::selectionClauses(input))
        if(!setup_source::forbiddenSelection(clause))text+=clause+";\n";
    const auto folded=detail::lowercase(text);const auto mask=setup_source::protectedValues(text);
    const auto values=[&](const char* category,const char* protocol,const char* key) {
        std::set<std::string> out;
        for(const auto& entry:candidate.value(category,Json::array())) if(entry.contains(protocol)&&entry.at(protocol).contains(key)) {
            const auto& value=entry.at(protocol).at(key);out.insert(value.is_string()?value.get<std::string>():value.dump());
        }
        return out;
    };
    if(folded.find("mqtt")!=std::string::npos) {
        auto topics=values("bindings","mqtt","topic");const auto observations=values("observations","mqtt","topic");topics.insert(observations.begin(),observations.end());
        const std::regex topic(R"((topic|топик|состояние)[^\n.;]{0,160}?\s([A-Za-z0-9_-]+(?:/[A-Za-z0-9_.-]+)+))");
        for(auto it=std::sregex_iterator(folded.begin(),folded.end(),topic);it!=std::sregex_iterator();++it) {
            if(mask[static_cast<std::size_t>(it->position(1))]||mask[static_cast<std::size_t>(it->position(2))])continue;
            auto declared=text.substr(static_cast<std::size_t>(it->position(2)),static_cast<std::size_t>(it->length(2)));
            const auto end=static_cast<std::size_t>(it->position(2)+it->length(2));
            // A sentence-ending period outside quotes is punctuation, not a
            // mandatory extra topic byte. An explicitly quoted dot stays exact.
            if(!topics.count(declared)&&!declared.empty()&&declared.back()=='.'&&
               (end==text.size()||std::isspace(static_cast<unsigned char>(text[end]))))declared.pop_back();
            if(!topics.count(declared))throw std::invalid_argument("Incomplete plan: an explicitly stated MQTT topic has no mapping or observation");
        }
        const std::regex payload(R"((?:^|[^A-Za-z0-9_])(payload)[ "']*[:=]?[ "']*([A-Za-z0-9_-]+))");
        const auto sent=values("bindings","mqtt","payload");
        for(auto it=std::sregex_iterator(folded.begin(),folded.end(),payload);it!=std::sregex_iterator();++it) {
            if(mask[static_cast<std::size_t>(it->position(1))])continue;
            const auto declared=text.substr(static_cast<std::size_t>(it->position(2)),static_cast<std::size_t>(it->length(2)));
            if(!sent.count(declared))throw std::invalid_argument("Incomplete plan: explicitly stated MQTT payload is missing");
        }
    }
    if(folded.find("http")!=std::string::npos) {
        const std::regex route(R"((post|put|get)[ \t]+(/[A-Za-z0-9_./?=&%-]+))");
        for(auto it=std::sregex_iterator(folded.begin(),folded.end(),route);it!=std::sregex_iterator();++it) {
            if(mask[static_cast<std::size_t>(it->position(1))])continue;
            auto method=it->str(1);std::transform(method.begin(),method.end(),method.begin(),[](unsigned char c){return std::toupper(c);});
            const auto path=text.substr(static_cast<std::size_t>(it->position(2)),static_cast<std::size_t>(it->length(2)));bool found=false;
            for(const auto& entry:candidate.value(method=="GET"?"observations":"bindings",Json::array()))
                if(entry.contains("http")&&entry.at("http").value("method",std::string())==method&&entry.at("http").value("path",std::string())==path)found=true;
            if(!found)throw std::invalid_argument("Incomplete plan: explicitly stated HTTP method/path is missing");
        }
    }
    if(folded.find("modbus")!=std::string::npos) {
        auto addresses=values("bindings","modbus","address");const auto observations=values("observations","modbus","address");addresses.insert(observations.begin(),observations.end());
        const std::regex address(R"((address|адрес регистра|адрес катушки)[ \t:=]+([0-9]+))");
        for(auto it=std::sregex_iterator(folded.begin(),folded.end(),address);it!=std::sregex_iterator();++it) {
            if(mask[static_cast<std::size_t>(it->position(1))])continue;
            if(!addresses.count(it->str(2)))throw std::invalid_argument("Incomplete plan: explicitly stated Modbus address is missing");
        }
        const auto written=values("bindings","modbus","value");
        const std::regex writeValue(R"((?:^|[^A-Za-z0-9_])(value)[ \t:=]+(true|false|-?[0-9]+)(?:$|[^A-Za-z0-9_]))");
        for(auto it=std::sregex_iterator(folded.begin(),folded.end(),writeValue);it!=std::sregex_iterator();++it) {
            if(mask[static_cast<std::size_t>(it->position(1))])continue;
            if(!written.count(it->str(2)))throw std::invalid_argument("Incomplete plan: explicitly stated Modbus write value is missing");
        }
    }
}
inline Json compactCatalog(config::Catalog catalog) {
    const auto value = Json::parse(catalog->canonicalJson());
    Json out = {{"zones",Json::array()},{"devices",Json::array()},{"connections",Json::array()}};
    for (const auto& room : value.at("rooms")) out["zones"].push_back(room.at("name"));
    for (const auto& device : value.at("devices")) {
        Json room = nullptr;
        for (const auto& r : value.at("rooms")) if (device.at("room_id") == r.at("id")) room = r.at("name");
        Json actions = Json::array();
        for (const auto& action : value.at("actions"))
            if (std::find(device.at("action_ids").begin(),device.at("action_ids").end(),action.at("id"))!=device.at("action_ids").end())
                actions.push_back(action.at("name"));
        out["devices"].push_back({{"zone",room},{"name",device.at("name")},{"actions",actions},{"state",device.value("state",Json::array())}});
    }
    for (auto connection : value.value("connections",Json::array())) { connection.erase("id"); out["connections"].push_back(connection); }
    return out;
}
inline Json rejection(std::string explanation) {
    return {{"rejected",true},{"status","ambiguous"},{"summary",std::move(explanation)},{"operations",Json::array()}};
}
// Scalar transport tuples reduce nesting for the small local model. This is a
// strict wire decoder, not JSON repair: all facts still pass the same original
// document, name-resolution and authoritative configuration validators below.
inline Json transportTuple(const Json& t) {
    if(!t.is_array()||t.empty()||!t[0].is_string())throw std::invalid_argument("Typed setup operation required");
    const auto kind=t[0].get<std::string>();
    const auto size=[&](std::size_t count){if(t.size()!=count)throw std::invalid_argument(kind+" expects "+std::to_string(count-1)+" arguments");};
    if(kind=="mqtt_connection") {
        size(6);Json mqtt={{"tls",t[4]}};if(!t[5].is_null())mqtt["credentials_id"]=t[5];
        return Json::array({"connection",t[1],{{"type","mqtt"},{"network",{{"host",t[2]},{"port",t[3]}}},{"mqtt",mqtt}}});
    }
    if(kind=="mqtt_binding") {
        size(9);return Json::array({"binding",t[1],t[2],t[3],t[4],{{"mqtt",{{"topic",t[5]},{"payload",t[6]},{"qos",t[7]},{"retain",t[8]}}}}});
    }
    if(kind=="mqtt_observe") {
        size(9);return Json::array({"observe",t[1],t[2],t[3],{{"mqtt",{{"topic",t[4]},{"format","text"},{"field",t[5]},{"true_payload",t[6]},{"false_payload",t[7]},{"qos",t[8]}}},{"fields",Json::object()}}});
    }
    if(kind=="http_connection") {
        size(4);return Json::array({"connection",t[1],{{"type","http"},{"http",{{"base_url",t[2]},{"base_path",t[3]}}}}});
    }
    if(kind=="http_binding") {
        size(9);return Json::array({"binding",t[1],t[2],t[3],t[4],{{"http",{{"method",t[5]},{"path",t[6]},{"body",t[7]},{"expected_status",t[8]}}}}});
    }
    if(kind=="http_observe") {
        size(7);return Json::array({"observe",t[1],t[2],t[3],{{"http",{{"method","GET"},{"path",t[4]},{"expected_status",t[6]}}},{"fields",t[5]}}});
    }
    if(kind=="modbus_tcp_connection") {
        size(5);return Json::array({"connection",t[1],{{"type","modbus_tcp"},{"network",{{"host",t[2]},{"port",t[3]}}},{"modbus",{{"unit_id",t[4]}}}}});
    }
    if(kind=="modbus_rtu_connection") {
        size(7);return Json::array({"connection",t[1],{{"type","modbus_rtu"},{"serial",{{"port",t[2]},{"baud_rate",t[3]},{"parity",t[4]},{"stop_bits",t[5]}}},{"modbus",{{"unit_id",t[6]}}}}});
    }
    if(kind=="modbus_binding"||kind=="modbus_argument") {
        size(9);return Json::array({"binding",t[1],t[2],t[3],t[4],{{"modbus",{{"function",t[5]},{"address",t[6]},{"data_type",t[7]},{kind=="modbus_binding"?"value":"argument",t[8]}}}}});
    }
    if(kind=="modbus_observe") {
        size(8);return Json::array({"observe",t[1],t[2],t[3],{{"modbus",{{"function",t[4]},{"address",t[5]},{"data_type",t[6]},{"field",t[7]}}},{"fields",Json::object()}}});
    }
    return t;
}
// Compact wire notation for a NEW power device. Expansion only removes repeated
// names: the resulting operations still go through all ordinary document,
// original-fact, schema and private-draft validators. It never executes I/O.
inline Json expandPowerSetups(const Json& tuples, config::Catalog catalog, std::set<std::string>* derivedNames=nullptr) {
    const auto original=Json::parse(catalog->canonicalJson());
    const auto existingConnections=original.value("connections",Json::array());
    std::map<std::string,std::set<std::string>> automatic;
    for(const auto& item:tuples) {
        if(item.is_object()&&item.value("op",std::string())=="power_setup"&&!item.contains("connection_name")&&item.at("connection").is_array())
            automatic[item.at("device")].insert(item.at("zone"));
        if(item.is_array()&&item.size()==7&&item[0]=="power_setup"&&item[2]==item[3]&&item[4].is_array())
            automatic[item[2]].insert(item[1]);
    }
    Json out=Json::array();std::set<std::string> zones;
    for(const auto& room:original.at("rooms"))zones.insert(detail::lowercase(room.at("name").get<std::string>()));
    for(const auto& item:tuples) {
        auto t=item;
        if(t.is_object()) {
            if(t.contains("connection_name"))members(t,{"op","zone","device","connection","connection_name","commands","observation"});
            else members(t,{"op","zone","device","connection","commands","observation"});
            if(t.at("op")!="power_setup")throw std::invalid_argument("Unsupported compact operation");
            const auto connection=t.at("connection");
            const auto name=t.value("connection_name",connection.is_string()?connection:t.at("device"));
            t=Json::array({"power_setup",t.at("zone"),t.at("device"),name,
                connection.is_string()?Json():connection,t.at("commands"),t.at("observation")});
        }
        if(!t.is_array()||t.empty()||t[0]!="power_setup") {
            out.push_back(t);
            if(t.is_array()&&t.size()==2&&t[0]=="location"&&t[1].is_string())zones.insert(detail::lowercase(t[1].get<std::string>()));
            continue;
        }
        if(t.size()!=7||!t[1].is_string()||!t[2].is_string()||!t[3].is_string())
            throw std::invalid_argument("power_setup expects zone, new device, connection name, connection, commands, observation");
        const bool defaultName=(item.is_object()&&!item.contains("connection_name"))||(item.is_array()&&t[3]==t[2]);
        const bool occupied=std::any_of(existingConnections.begin(),existingConnections.end(),[&](const Json& c) {
            return detail::lowercase(c.at("name").get<std::string>())==detail::lowercase(t[3].get<std::string>());
        });
        if(defaultName&&t[4].is_array()&&t[3]==t[2]&&(automatic[t[2]].size()>1||occupied)) {
            t[3]=t[2].get<std::string>()+" · "+t[1].get<std::string>();
            if(derivedNames)derivedNames->insert(t[3]);
        }
        for(const auto& device:original.at("devices"))if(device.at("name")==t[2])
            for(const auto& room:original.at("rooms"))if(device.at("room_id")==room.at("id")&&room.at("name")==t[1])
                throw std::invalid_argument("power_setup creates new devices only; existing devices require explicit operations");
        if(zones.insert(detail::lowercase(t[1].get<std::string>())).second)out.push_back(Json::array({"location",t[1]}));
        out.push_back(Json::array({"power_device",t[1],t[2]}));
        const auto& c=t[4];const auto& b=t[5];const auto& o=t[6];
        const auto shape=[](const Json& v,std::size_t n){if(!v.is_array()||v.size()!=n||!v[0].is_string())throw std::invalid_argument("Invalid compact power transport tuple");};
        if(!c.is_null()) {
            for(const auto& old:original.value("connections",Json::array()))
                if(detail::lowercase(old.at("name").get<std::string>())==detail::lowercase(t[3].get<std::string>()))
                    throw std::invalid_argument("Compact creation cannot replace an existing connection; reuse its name without settings or request a new name");
            if(!c.is_array()||c.empty()||!c[0].is_string())throw std::invalid_argument("Invalid compact connection");
            Json flat=Json::array({c[0].get<std::string>()+"_connection",t[3]});
            const auto type=c[0].get<std::string>();
            const auto n=type=="mqtt"?6:type=="http"?4:type=="modbus_tcp"?5:type=="modbus_rtu"?7:0;
            if(!n)throw std::invalid_argument("Unsupported compact connection");
            shape(c,n);
            for(std::size_t i=1;i+1<c.size();++i)flat.push_back(c[i]);
            auto connection=transportTuple(flat);connection.back()["timeout_ms"]=c.back();out.push_back(connection);
        } else {
            bool found=false;for(const auto& connection:original.value("connections",Json::array()))found=found||connection.at("name")==t[3];
            if(!found)throw std::invalid_argument("Unknown existing connection");
        }
        if(!b.is_array()||b.empty()||!b[0].is_string())throw std::invalid_argument("Power commands required");
        const auto type=b[0].get<std::string>();
        for(int action=0;action<2;++action) {
            Json flat=Json::array({type+"_binding",t[1],t[2],action==0?"ON":"OFF",t[3]});
            if(type=="mqtt") {shape(b,7);flat.push_back(b[action?3:1]);flat.push_back(b[action?4:2]);flat.push_back(b[5]);flat.push_back(b[6]);}
            else if(type=="http") {
                shape(b,7);flat.push_back(b[1]);flat.push_back(b[action?4:2]);
                const auto& encoded=b[action?5:3];
                const auto body=encoded.is_string()?Json::parse(encoded.get<std::string>()):encoded;
                if(!body.is_object())throw std::invalid_argument("HTTP body must be an object or its strict JSON encoding");
                flat.push_back(body);flat.push_back(b[6]);
            }
            else if(type=="modbus") {shape(b,6);for(int i=1;i<=3;++i)flat.push_back(b[i]);flat.push_back(b[action?5:4]);}
            else throw std::invalid_argument("Unsupported compact commands");
            out.push_back(flat);
        }
        if(!o.is_null()) {
            Json flat=Json::array({type+"_observe",t[1],t[2],t[3]});
            if(type=="mqtt") {shape(o,5);if(o[0]!="mqtt")throw std::invalid_argument("Observation transport mismatch");flat.push_back(o[1]);flat.push_back("power");for(int i=2;i<5;++i)flat.push_back(o[i]);}
            else if(type=="http") {shape(o,4);if(o[0]!="http")throw std::invalid_argument("Observation transport mismatch");for(int i=1;i<4;++i)flat.push_back(o[i]);}
            else {shape(o,4);if(o[0]!="modbus")throw std::invalid_argument("Observation transport mismatch");for(int i=1;i<4;++i)flat.push_back(o[i]);flat.push_back("power");}
            out.push_back(flat);
        }
        if(out.size()>64)throw std::invalid_argument("Expanded setup operation limit exceeded");
    }
    return out;
}

inline std::vector<setup_source::Target> setupTargets(const Json& operations) {
    std::vector<setup_source::Target> result;
    for(const auto& t:operations)if(t.is_array()&&t.size()==3&&(t[0]=="device"||t[0]=="power_device"))
        result.push_back({t[1].is_null()?std::string():t[1].get<std::string>(),t[2]});
    return result;
}
struct SourceHttpEndpoint {std::string host;int port;};
inline SourceHttpEndpoint sourceHttpEndpoint(std::string_view uri) {
    if(uri.substr(0,7)!="http://")throw std::invalid_argument("Invalid HTTP endpoint");
    const auto end=uri.find_first_of("/?#",7);
    const auto authority=std::string(uri.substr(7,end==std::string_view::npos?end:end-7));std::smatch match;
    if(!std::regex_match(authority,match,std::regex(R"(([A-Za-z0-9_.-]+)(?::([0-9]+))?)")))
        throw std::invalid_argument("Укажите полный HTTP host и целый port; дробный/неполный порт недопустим.");
    const auto port=match[2].matched?std::stoll(match.str(2)):80;
    if(port<1||port>65535)throw std::invalid_argument("HTTP port вне диапазона 1..65535.");
    return {detail::lowercase(match.str(1)),static_cast<int>(port)};
}
inline std::vector<SourceHttpEndpoint> sourceHttpEndpoints(std::string_view source) {
    std::vector<SourceHttpEndpoint> result;const auto mask=setup_source::protectedValues(source);
    // A quoted URI is an endpoint only as the value of an endpoint field,
    // never merely because a state payload happens to look like a URI.
    for(const auto& value:setup_source::assignedValues(source,"base_url|url|адрес|http"))
        if(value.find("http://")==0)result.push_back(sourceHttpEndpoint(value));
    for(std::size_t at=0;(at=source.find("http://",at))!=std::string_view::npos;) {
        if(mask[at]) {at+=7;continue;}
        auto end=at;
        while(end<source.size()&&!std::isspace(static_cast<unsigned char>(source[end]))&&
              std::string_view(";\"'<>)]}").find(source[end])==std::string_view::npos) {
            if(source[end]==','&&(end+1==source.size()||!std::isdigit(static_cast<unsigned char>(source[end+1]))))break;
            ++end;
        }
        result.push_back(sourceHttpEndpoint(source.substr(at,end-at)));at=end;
    }
    return result;
}
inline void requireSelectedEndpoint(const Json& connection,const std::string& name,std::string_view source,bool preservedReference=false) {
    const auto text=setup_source::labels(std::string(source));
    auto endpointText=text;const auto jsonMask=setup_source::protectedValues(source,true);
    const auto quotedMask=setup_source::protectedValues(source);
    for(std::size_t i=0;i<endpointText.size();++i)if(jsonMask[i])endpointText[i]=' ';
    std::set<std::string> hosts;
    const std::regex hostField(R"((?:^|[ \t\r\n;,(])(host|hostname|адрес брокера|адрес контроллера)[ \t"':=]+([A-Za-z0-9_.-]+))");
    for(auto i=std::sregex_iterator(endpointText.begin(),endpointText.end(),hostField);i!=std::sregex_iterator();++i)
        if(!quotedMask[static_cast<std::size_t>(i->position(1))])hosts.insert(i->str(2));
    const std::regex brokerField(R"((?:^|[ \t\r\n;,(])(брокер)[ \t"':=]+([A-Za-z0-9_.-]+))");
    for(auto i=std::sregex_iterator(endpointText.begin(),endpointText.end(),brokerField);i!=std::sregex_iterator();++i) {
        if(quotedMask[static_cast<std::size_t>(i->position(1))])continue;
        // "Брокер: topic ..." references an existing connection named Брокер.
        // Its following setting key is not a DNS host. Explicit host/hostname
        // declarations are scanned separately, including "Брокер host ...".
        static const std::set<std::string> settings={"topic","qos","tls","format","field","true_payload","false_payload","credentials_id","host","hostname","port","timeout_ms"};
        if(connection.contains("id")&&setup_source::labels(name)=="брокер"&&settings.count(i->str(2)))continue;
        hosts.insert(i->str(2));
    }
    const std::regex address(R"((?:адрес|mqtt|modbus tcp)[ \t"':=]+([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+))");
    for(auto i=std::sregex_iterator(endpointText.begin(),endpointText.end(),address);i!=std::sregex_iterator();++i)
        if(!quotedMask[static_cast<std::size_t>(i->position())])hosts.insert(i->str(1));
    const auto sourceUris=sourceHttpEndpoints(source);
    for(const auto& uri:sourceUris)hosts.insert(uri.host);
    const auto requireNumber=[&](const std::vector<int>& values,const Json& actual,const char* field) {
        const std::set<int> declared(values.begin(),values.end());
        if(!declared.empty()&&(declared.size()!=1||actual!=*declared.begin()))throw std::invalid_argument(std::string("Explicit endpoint field mismatch: ")+field);
    };
    const auto [requestedTypes,forbiddenTypes]=setup_source::transports(source);
    if(forbiddenTypes.count(connection.at("type"))||(!requestedTypes.empty()&&(requestedTypes.size()!=1||!requestedTypes.count(connection.at("type")))))
        throw std::invalid_argument("unstated_connection_transport: протокол выбранного подключения не соответствует устройству.");
    const bool endpoint=!hosts.empty()||endpointText.find("/dev/")!=std::string::npos;
    if(!endpoint&&!preservedReference&&!setup_source::mentions(source,name))
        throw std::invalid_argument("unstated_connection_reference: укажите подключение или endpoint этого устройства.");
    if(connection.contains("network")) {
        const auto& network=connection.at("network");
        if(hosts.empty()&&!preservedReference&&(!connection.contains("id")||!setup_source::mentions(source,name)))
            throw std::invalid_argument("Укажите host именно этого подключения, а не адрес внутри payload.");
        if(!hosts.empty()&&(hosts.size()!=1||network.at("host")!=*hosts.begin()))throw std::invalid_argument("Explicit endpoint host mismatch");
        if(endpoint)fact(network.at("host"),Json(),source,"network.host");
        requireNumber(setup_source::fieldNumbers(endpointText,"(?:port|порт)"),network.at("port"),"port");
    }
    if(connection.contains("http")&&endpoint) {
        facts(Json{{"http",{{"base_url",connection.at("http").at("base_url")}}}},Json(),source);
        const auto base=connection.at("http").at("base_url").get<std::string>();
        const auto selected=sourceHttpEndpoint(base);
        if(hosts.empty()&&!preservedReference&&(!connection.contains("id")||!setup_source::mentions(source,name)))throw std::invalid_argument("Explicit HTTP endpoint required");
        if(!hosts.empty()&&(hosts.size()!=1||!hosts.count(selected.host)))throw std::invalid_argument("Explicit HTTP host mismatch");
        requireNumber(setup_source::fieldNumbers(endpointText,"(?:port|порт)"),selected.port,"port");
        for(const auto& declared:sourceUris)if(declared.host!=selected.host||declared.port!=selected.port)
            throw std::invalid_argument("unstated_connection_endpoint: HTTP host/port не соответствует явно указанному URL.");
    }
    if(connection.contains("mqtt")) {
        std::smatch tls;
        if(std::regex_search(text,tls,std::regex(R"((?:^|[ \t\r\n;])tls[ \t:=]*(true|false|включен|включён|выключен|выключён)(?:[ \t\r\n,.;]|$))"))) {
            const bool requested=tls.str(1)=="true"||tls.str(1)=="включен"||tls.str(1)=="включён";
            if(connection.at("mqtt").value("tls",false)!=requested)throw std::invalid_argument("Explicit MQTT TLS setting mismatch");
        }
    }
    if(connection.contains("serial")) {
        const auto& serial=connection.at("serial");
        if(endpointText.find("/dev/")==std::string::npos&&!connection.contains("id"))throw std::invalid_argument("Укажите последовательный порт этого устройства явно.");
        if(endpoint)fact(serial.at("port"),Json(),source,"serial.port");
        requireNumber(setup_source::fieldNumbers(endpointText,"(?:baud_rate|baud|скорость)"),serial.at("baud_rate"),"baud_rate");
        if(text.find("parity")!=std::string::npos||text.find("чётност")!=std::string::npos||text.find("четност")!=std::string::npos)
            fact(serial.at("parity"),Json(),source,"serial.parity");
        auto stop=setup_source::fieldNumbers(source,"stop_bits");
        if(text.find("один стоп")!=std::string::npos)stop.push_back(1);
        if(text.find("два стоп")!=std::string::npos)stop.push_back(2);
        if(!stop.empty()&&(std::set<int>(stop.begin(),stop.end()).size()!=1||serial.at("stop_bits")!=stop.front()))
            throw std::invalid_argument("Explicit serial stop bits mismatch");
    }
    if(connection.contains("modbus"))requireNumber(setup_source::fieldNumbers(endpointText,"(?:unit_id|unit id|slave_id)"),connection.at("modbus").at("unit_id"),"unit_id");
}
inline Json powerCommand(const Json& settings) {
    Json out=Json::object();
    for(const std::string type:{"mqtt","http","modbus"})if(settings.contains(type)) {
        const auto& s=settings.at(type);Json fields=Json::object();
        for(const std::string key:{"topic","payload","method","path","body","function","address","data_type","value"})
            if(s.contains(key))fields[key]=s.at(key);
        out[type]=fields;
    }
    return out;
}
// Both the staged and single-response paths use this same source contract.
// The final decoder still owns authoritative schema/domain validation.
inline void validatePowerSources(const Json& operations,config::Catalog catalog,std::string_view input,
                                 std::vector<setup_source::Target> targets={},bool inventory=true) {
    const auto original=Json::parse(catalog->canonicalJson());
    const auto created=setupTargets(operations);
    if(targets.empty())targets=created;
    std::vector<std::string> zones;
    for(const auto& room:original.at("rooms"))zones.push_back(room.at("name"));
    for(const auto& device:original.at("devices")) {
        std::string zone;for(const auto& room:original.at("rooms"))if(device.at("room_id")==room.at("id"))zone=room.at("name");
        setup_source::Target target{zone,device.at("name")};
        if(std::none_of(targets.begin(),targets.end(),[&](const auto& t){return setup_source::equal(t,target);}))targets.push_back(target);
    }
    const setup_source::Document document(input,targets,zones);
    if(inventory)document.requireInventory(created);
    std::map<std::string,Json> connections;
    for(const auto& c:original.value("connections",Json::array()))connections[c.at("name")]=c;
    for(const auto& t:operations)if(t.is_array()&&t.size()==3&&t[0]=="connection")connections[t[1]]=t.back();
    std::map<std::string,Json> reads;std::map<std::string,setup_source::Target> usedModbus;
    for(const auto& t:operations)if(t.is_array()&&t.size()==5&&t[0]=="observe") {
        setup_source::Target target{t[1].is_null()?std::string():t[1].get<std::string>(),t[2]};
        const auto name=t[3].get<std::string>();const auto source=document.forTarget(target);
        if(!connections.count(name))throw std::invalid_argument("Unknown observation connection reference");
        bool preservedReference=false;
        if(t.back().contains("modbus")) {
            setup_source::requireModbusRead(t.back().at("modbus"),source);
            reads[target.key()]=t.back().at("modbus");
        }
        if(t.back().contains("mqtt")) {
            setup_source::requireMqttQos(t.back().at("mqtt").value("qos",0),source,true);
            auto mapping=t.back().at("mqtt");mapping.erase("qos");bool unchanged=false;
            for(const auto& device:original.at("devices"))if(setup_source::same(device.at("name").get<std::string>(),target.name)) {
                std::string zone;for(const auto& room:original.at("rooms"))if(device.at("room_id")==room.at("id"))zone=room.at("name");
                if(!setup_source::same(zone,target.zone))continue;
                for(const auto& prior:original.value("observations",Json::array()))
                    if(prior.at("target_id")==device.at("id")&&prior.value("connection_id",std::string())==connections.at(name).value("id",std::string())&&prior.contains("mqtt")) {
                        auto old=prior.at("mqtt");old.erase("qos");unchanged=unchanged||old==mapping;
                        if(old==mapping&&prior.value("fields",Json::object())==t.back().value("fields",Json::object()))
                            for(const auto& c:original.value("connections",Json::array()))
                                if(c==connections.at(name))preservedReference=true;
                    }
            }
            const auto read=setup_source::mqttReadFacts(source);const auto folded=setup_source::instructionText(read);
            const bool statedMapping=!setup_source::quotedAssignments(read,"true_payload|false_payload").empty()||folded.find("означает")!=std::string::npos||folded.find("true_payload")!=std::string::npos||folded.find("false_payload")!=std::string::npos||folded.find("on/off")!=std::string::npos;
            if(!unchanged||statedMapping) {
                setup_source::requireMqttRead(t.back().at("mqtt"),source);
                fact(t.back().at("mqtt").at("topic"),Json(),setup_source::mqttReadFacts(source),"state.mqtt.topic");
            }
        }
        // Catalog identity only supplies an omitted reference for an otherwise
        // identical MQTT observation (apart from QoS). Explicit source endpoint
        // and transport constraints still run; no endpoint/mapping change inherits it.
        requireSelectedEndpoint(connections.at(name),name,source,preservedReference);
    }
    for(const auto& t:operations) {
        if(!t.is_array()||t.size()!=6||t[0]!="binding")continue;
        setup_source::Target target{t[1].is_null()?std::string():t[1].get<std::string>(),t[2]};
        const auto source=document.forTarget(target);const auto& settings=t.back();const auto name=t[4].get<std::string>();
        if(!connections.count(name))throw std::invalid_argument("Unknown connection reference");
        requireSelectedEndpoint(connections.at(name),name,source);
        if(settings.contains("mqtt"))setup_source::requireMqttQos(settings.at("mqtt").value("qos",0),source,false);
        if(settings.contains("modbus"))usedModbus[target.key()]=target;
        int power=-1;const auto action=detail::lowercase(t[3].get<std::string>());
        if(action=="on"||action=="включить")power=1;
        if(action=="off"||action=="выключить")power=0;
        Json prior;
        for(const auto& device:original.at("devices"))if(setup_source::same(device.at("name").get<std::string>(),target.name)) {
            std::string zone;for(const auto& room:original.at("rooms"))if(device.at("room_id")==room.at("id"))zone=room.at("name");
            if(!setup_source::same(zone,target.zone))continue;
            for(const auto& a:original.at("actions"))if(std::find(device.at("action_ids").begin(),device.at("action_ids").end(),a.at("id"))!=device.at("action_ids").end()) {
                const auto assignments=a.value("assignments",Json::array());
                if(assignments.size()!=1||assignments[0].value("field",std::string())!="power"||!assignments[0].at("value").is_boolean())continue;
                bool selected=t[3]==a.at("id")||t[3]==a.at("name")||(power>=0&&assignments[0].at("value")==static_cast<bool>(power));
                if(!selected)continue;
                power=assignments[0].at("value").get<bool>()?1:0;
                for(const auto& b:original.value("bindings",Json::array()))if(b.at("target_id")==device.at("id")&&b.at("action_id")==a.at("id"))prior=b;
            }
        }
        if(power<0)continue;
        if(!prior.is_null()&&powerCommand(prior)==powerCommand(settings))continue;
        const auto actionSource=setup_source::actionFacts(source,power==1);
        if(settings.contains("mqtt")) {
            setup_source::requirePowerValue(settings.at("mqtt").at("payload"),actionSource,true);
            if(setup_source::mentions(actionSource,"topic")||setup_source::mentions(actionSource,"топик"))
                fact(settings.at("mqtt").at("topic"),Json(),actionSource,"action.mqtt.topic");
        }
        if(settings.contains("http")) {
            setup_source::requireHttpBody(settings.at("http").at("body"),actionSource);
            facts(Json{{"http",settings.at("http")}},Json(),actionSource);
        }
        if(settings.contains("modbus"))setup_source::requireModbusWrite(settings.at("modbus"),source,power==1);
    }
    for(const auto& entry:usedModbus)if(!reads.count(entry.first))
        setup_source::requireModbusRead(Json(),document.forTarget(entry.second));
}
}

inline std::string setupDocumentPrompt(config::Catalog catalog, std::string_view input = {}, const Json& focus=Json::array(), const std::string& contextLocation={}) {
    const auto text=detail::lowercase(input);
    const bool mqtt=text.find("mqtt")!=std::string::npos;
    const bool http=text.find("http")!=std::string::npos;
    const bool modbus=text.find("modbus")!=std::string::npos;
    std::string prompt=R"(You plan Firefly configuration, never execute commands. Read ALL of INPUT_DATA.text, including its final restrictions. CATALOG is the existing configuration. Keep user names in their language; Russian names use nominative case. Do not translate or invent names, endpoints, capabilities, schedules or automation.
Return exactly ONE valid JSON object, without markdown or commentary. For a complete plan use complete:true and ops, an array of operation arrays. If facts are missing use only a question field with a specific short Russian question. For unsupported work use only an unsupported field with a short Russian reason. Never combine these alternatives. Do not return a partial plan. At most 12 operations.
One example of valid output syntax (use the user's names instead): {"complete":true,"ops":[["location","Павильон"],["power_device","Павильон","Обогреватель"]]}
Allowed operations, with exact positional arguments:
["location",zone_name] creates a zone, unless it already exists.
["power_device",zone_name,device_name] creates a device with exactly ON and OFF.
["device",zone_name,device_name] creates a device with NO actions, only when capabilities were not requested.
The phrases включать и выключать, включение и выключение, включается и выключается and ON/OFF request power_device. A final rule for оба/все/всех applies to all named devices. Do not omit their requested power capability. Use null for zone only when explicitly без зоны.
["system_name",new_name] only if explicitly renaming the system.
["rename_location",old_name,new_name,keep_old_alias_boolean]
["rename_device",zone_name,old_name,new_name]
["delete_device",zone_name,device_name]
["add_brightness",zone_name,device_name] only explicitly requested brightness.
["bounds",zone_name,device_name,field_name,minimum_number,maximum_number]
Make only the requested changes. Respect exclusions. A description such as у меня кухня и там лампа asks to create those named objects. Create zones before devices. Keep separate devices in different zones even if their names match. Ask when a referent is ambiguous. Never Save or Apply.)";
    if(!mqtt&&!http&&!modbus)prompt+="\nNo transport is requested: prepare logical devices only. Do not ask for IP or connection parameters.";
    else {
        // Do not show the small model unrelated rename/delete examples before
        // a connected-device task: it used to copy them into an invalid plan.
        prompt=R"(You configure Firefly, never execute devices. Read the ENTIRE INPUT_DATA.text, including exclusions. Return ONE strict JSON object without markdown, explanation or thinking.
If information is missing return {"question":"specific short Russian question"}. If unsupported return {"unsupported":"short Russian reason"}. Otherwise return complete=true and an ops array, at most 12 operations. Never output a partial plan. Preserve ALL existing CATALOG objects.
For each NEW device with ON/OFF use ONE object with these keys: op="power_setup", zone=exact zone name, device=exact device name, connection, commands, observation. This one object creates the zone if missing, device, connection, ON/OFF commands and optional readback. Do not emit extra zone/device operations.
A complete JSON syntax example for ONE fictitious MQTT device, NOT user facts:
{"complete":true,"ops":[{"op":"power_setup","zone":"Example room","device":"Example lamp","connection":["mqtt","example.invalid",1883,false,null,2000],"commands":["mqtt","example/set","ON","example/set","OFF",0,false],"observation":null}]}
For multiple devices put multiple complete objects into ops, separated by commas. Every object member has a quoted key, a colon and a value. Do not output positional values inside curly braces. Arrays use square brackets; no parentheses. Copy the actual names and values from INPUT_DATA, never copy example values.
The connection value is either an array for a NEW connection or the exact name string of a matching existing CATALOG connection. Never modify an existing connection to fit a new endpoint. A new connection is named after the new device automatically; do not invent a connection name. Use optional connection_name only if the user explicitly named a new connection.
Connection arrays (timeout in milliseconds, default 2000):
["mqtt",host,port,tls,credentials_id_or_null,timeout]
["http",base_url,base_path,timeout]
["modbus_tcp",host,port,unit_id,timeout]
["modbus_rtu",serial_port,baud_rate,parity,stop_bits,unit_id,timeout]
commands is one array, always ON then OFF:
["mqtt",on_topic,on_payload,off_topic,off_payload,qos,retain]
["http",method,on_path,on_body_object,off_path,off_body_object,status]
["modbus",write_function,address,data_type,on_value,off_value]
observation is null if no readback was supplied, otherwise:
["mqtt",state_topic,on_payload,off_payload,qos]
["http",GET_path,fields_object,status]
["modbus",read_function,address,data_type]
Keep every numeric value exactly as specified, including address 0. unit_id belongs to connection; address belongs to commands and observation. Modbus functions 1/2/5 use data_type "boolean" and JSON true/false; functions 3/4/6 require explicit uint16/int16. RTU parity none/even/odd; one stop bit means 1. Eight data bits are fixed, not an extra array element.
MQTT payloads are strings with exact case: ON is not on. HTTP bodies are JSON objects, not strings: e.g. {"on":true}. HTTP base_url may combine explicit host and port with http://. Defaults: qos=0, retain=false, tls=false, base_path="", status=200. credentials_id=null when unspecified; never invent credentials. Never invent state readback or treat delivery as physical state.
For existing devices only, use the original typed operations: ["connection",name,settings_object], ["binding",zone,device,"ON" or "OFF",connection_name,settings_object], ["observe",zone,device,connection_name,settings_object]. Preserve their catalog schema; if uncertain ask. Never replace an existing device with power_setup.
Return the full plan as valid JSON. Names stay in the user's language.)";
    }
    prompt+="\nCATALOG="+setup_document::compactCatalog(catalog).dump();
    if(!focus.empty()||!contextLocation.empty()) {
        const auto full=Json::parse(catalog->canonicalJson());Json selected=Json::array();
        for(const auto& room:full.at("rooms"))if(room.at("id")==contextLocation||std::find(focus.begin(),focus.end(),room.at("id"))!=focus.end())selected.push_back({{"zone",room.at("name")}});
        for(const auto& device:full.at("devices"))if(std::find(focus.begin(),focus.end(),device.at("id"))!=focus.end()) {
            Json zone=nullptr;for(const auto& room:full.at("rooms"))if(room.at("id")==device.at("room_id"))zone=room.at("name");
            selected.push_back({{"device",device.at("name")},{"zone",zone}});
        }
        prompt+="\nFOCUS="+selected.dump()+"\nМестоимения там/его относятся к FOCUS только при единственной подходящей цели. Иначе уточни. Явно названная цель важнее FOCUS.";
    }
    return prompt;
}

inline std::string decodeSetupDocument(std::string_view raw, config::Catalog catalog, std::string_view input, const Json& focus = Json::array(), const std::string& contextLocation={}) {
    validateSetupDocumentInput(input);
    if (raw.size()>16384) throw std::invalid_argument("Setup document result too large");
    auto wire=parseMessage(raw,true);
    if(wire.contains("question")||wire.contains("unsupported"))
        return decodeSetupCopilotProposal(expandDialogueOperations(raw).dump(),catalog);
    members(wire,wire.contains("complete")?std::set<std::string>{"ops","complete"}:std::set<std::string>{"ops"});
    if(!wire.at("ops").is_array()||wire.at("ops").empty()||wire.at("ops").size()>12)
        throw std::invalid_argument("Complete plan requires 1..12 operations; never truncate a larger request");
    std::set<std::string> derivedConnectionNames;
    wire["ops"]=setup_document::expandPowerSetups(wire["ops"],catalog,&derivedConnectionNames);
    for(auto& operation:wire["ops"])operation=setup_document::transportTuple(operation);
    const bool connectionPlan=std::any_of(wire.at("ops").begin(),wire.at("ops").end(),[](const Json& op){return op.is_array()&&!op.empty()&&op[0].is_string()&&(op[0]=="connection"||op[0]=="binding"||op[0]=="observe");});
    if(connectionPlan||setup_document::named(input,"MQTT")||setup_document::named(input,"HTTP")||setup_document::named(input,"Modbus"))
        setup_document::validatePowerSources(wire["ops"],catalog,input);
    if ((input.size()>1800||connectionPlan||wire.contains("complete")) &&
        (!wire.contains("complete")||!wire.at("complete").is_boolean()||wire.at("complete")!=true))
        throw std::invalid_argument("incomplete_setup_document: модель должна подтвердить обработку всего запроса; частичный план отклонён");
    const auto folded=detail::lowercase(input);
    const bool deferred=folded.find("не настраива")!=std::string::npos||folded.find("без подключ")!=std::string::npos||
        folded.find("подключений пока не")!=std::string::npos||folded.find("подключения потом")!=std::string::npos||folded.find("пока не подключ")!=std::string::npos;
    if(!connectionPlan&&!deferred&&folded.find("подключ")!=std::string::npos&&
       (setup_document::named(input,"MQTT")||setup_document::named(input,"HTTP")||setup_document::named(input,"Modbus")))
        throw std::invalid_argument("Incomplete plan: requested connection is missing; ask for its missing facts instead of returning a semantic-only plan");
    if(connectionPlan && (folded.find("без подключений")!=std::string::npos||folded.find("не настраивай подключ")!=std::string::npos||folded.find("подключения потом")!=std::string::npos))
        throw std::invalid_argument("Connection configuration excluded by the original request");
    const auto original=Json::parse(catalog->canonicalJson());
    Json operations=Json::array();std::set<std::string> distinct;std::vector<std::string> deviceNames;
    for(const auto& device:original.at("devices"))deviceNames.push_back(device.at("name"));
    for(const auto& tuple:wire.at("ops"))if(tuple.is_array()&&tuple.size()==3&&(tuple[0]=="device"||tuple[0]=="power_device")&&tuple[2].is_string())deviceNames.push_back(tuple[2]);
    std::set<std::string> previousTargets;
    for(const auto& clause:setup_document::clauses(input)) {
        const auto targets=setup_document::mentionedTargets(clause,deviceNames);
        const bool singular=setup_document::named(clause,"его")||setup_document::named(clause,"её")||setup_document::named(clause,"ему");
        if(singular&&setup_document::powerPair(clause)&&(targets.size()>1||(targets.empty()&&previousTargets.size()>1)))
            return setup_document::rejection("Какому устройству добавить включение и выключение? Укажите его название.").dump();
        if(!targets.empty())previousTargets=targets;
    }
    for(const auto& tuple:wire.at("ops")) {
        if(!tuple.is_array()||tuple.empty()||!tuple[0].is_string())throw std::invalid_argument("Typed setup operation required");
        const auto kind=tuple[0].get<std::string>();Json op;
        if(kind=="connection"||kind=="binding"||kind=="observe") {
            const std::size_t size=kind=="connection"?3:kind=="binding"?6:5;
            if(tuple.size()!=size||!tuple.back().is_object())throw std::invalid_argument("Invalid typed connection operation");
            const auto nameIndex=kind=="connection"?1:kind=="binding"?4:3;
            if(!tuple[nameIndex].is_string()||tuple[nameIndex].get_ref<const std::string&>().empty())throw std::invalid_argument("Connection name required");
            const auto name=tuple[nameIndex].get<std::string>();Json prior;
            for(const auto& connection:original.value("connections",Json::array()))
                if(detail::lowercase(connection.at("name").get<std::string>())==detail::lowercase(name))prior=connection;
            if(kind=="connection") {
                if(!setup_document::named(input,name)&&prior.is_null()&&!derivedConnectionNames.count(name))throw std::invalid_argument("Connection name was not supplied");
                setup_document::facts(tuple.back(),prior,input);
                op={{"op","setup_connection"},{"name",name},{"settings",tuple.back()}};
            } else {
                if(!setup_document::targetMentioned(input,original,focus,tuple[2],"devices"))
                    throw std::invalid_argument("Unstated or ambiguous connection target");
                Json existing;
                if(!prior.is_null()) {
                    for(const auto& mapping:original.value(kind=="binding"?"bindings":"observations",Json::array()))
                        if(mapping.value("connection_id",Json())==prior.at("id")) {
                            // Reuse only facts of the selected device AND action.
                            // ON must never borrow an existing OFF payload.
                            for(const auto& device:original.at("devices"))if(device.at("id")==mapping.at("target_id")&&(tuple[2]==device.at("name")||tuple[2]==device.at("id"))) {
                                bool zone=tuple[1].is_null()&&device.at("room_id").is_null();
                                for(const auto& room:original.at("rooms"))if(room.at("id")==device.at("room_id")&&(tuple[1]==room.at("id")||tuple[1]==room.at("name")))zone=true;
                                if(!zone)continue;
                                bool actionMatches=kind=="observe";
                                if(kind=="binding")for(const auto& action:original.at("actions"))if(action.at("id")==mapping.at("action_id")) {
                                    actionMatches=tuple[3]==action.at("id")||tuple[3]==action.at("name");
                                    for(const auto& alias:action.value("speech_aliases",Json::array()))actionMatches=actionMatches||tuple[3]==alias;
                                    const auto assignments=action.value("assignments",Json::array());
                                    if((tuple[3]=="ON"||tuple[3]=="OFF")&&assignments.size()==1&&assignments[0].value("field",std::string())=="power"&&assignments[0].at("value").is_boolean())
                                        actionMatches=assignments[0].at("value").get<bool>()==(tuple[3]=="ON");
                                }
                                if(actionMatches)existing=mapping;
                            }
                        }
                }
                setup_document::facts(tuple.back(),existing,input);
                op={{"op",kind=="binding"?"setup_binding":"setup_observe"},{"location",tuple[1]},{"device",tuple[2]},{"connection",name},{"settings",tuple.back()}};
                if(kind=="binding")op["action"]=tuple[3];
            }
        } else {
            op=expandDialogueOperations(Json{{"ops",Json::array({tuple})}}.dump()).at("operations")[0];
            if(kind=="system_name" && (folded.find("систем")==std::string::npos ||
               (folded.find("назов")==std::string::npos&&folded.find("переимен")==std::string::npos&&folded.find("назван")==std::string::npos)))
                throw std::invalid_argument("System rename was not explicitly requested");
            if(kind=="power_device") {
                if(!setup_document::powerFor(input,op.at("name").get<std::string>(),deviceNames))op["template"]="custom"; // No capabilities inherited from another device.
            }
            if(kind=="device" && setup_document::powerFor(input,op.at("name").get<std::string>(),deviceNames))
                throw std::invalid_argument("Explicit ON/OFF missing; use power_device for the requested capabilities");
            if(kind=="rename_device"||kind=="delete_device"||kind=="add_brightness"||kind=="bounds") {
                if(!setup_document::targetMentioned(input,original,focus,op.at("device"),"devices"))
                    throw std::invalid_argument("Unstated or ambiguous mutation target; name the device and zone");
                if(kind=="delete_device"&&folded.find("удал")==std::string::npos&&folded.find("убери")==std::string::npos&&folded.find("не добавляй")==std::string::npos)
                    throw std::invalid_argument("Device deletion was not requested");
                if(kind=="rename_device"&&folded.find("назов")==std::string::npos&&folded.find("переимен")==std::string::npos&&folded.find("назван")==std::string::npos)
                    throw std::invalid_argument("Device rename was not requested");
                if(kind=="bounds") {
                    setup_document::fact(op.at("minimum"),Json(),input,"minimum");setup_document::fact(op.at("maximum"),Json(),input,"maximum");
                }
            }
            if(kind=="rename_location"&&!setup_document::targetMentioned(input,original,focus,op.at("location"),"rooms"))
                throw std::invalid_argument("Unstated location mutation target");
            if(kind=="add_brightness"&&(folded.find("ярк")==std::string::npos||folded.find("без ярк")!=std::string::npos||folded.find("не добавляй ярк")!=std::string::npos))
                throw std::invalid_argument("Brightness is absent or excluded by the original request");
            if(kind=="device"||kind=="power_device"||kind=="location") {
                const auto name=op.at("name").get<std::string>();
                if(kind=="location")for(const auto& room:original.at("rooms"))if(detail::lowercase(room.at("name").get<std::string>())==detail::lowercase(name))
                    throw std::invalid_argument("Location already exists; refer to it instead of creating a duplicate");
                const bool ascii=std::all_of(name.begin(),name.end(),[](unsigned char c){return c<128;});
                const bool localized=std::any_of(input.begin(),input.end(),[](unsigned char c){return c>=128;});
                if(ascii&&localized&&!setup_document::named(input,name))throw std::invalid_argument("Use the user's names, not invented IDs/transliteration");
                if((kind=="device"||kind=="power_device")&&op.at("location").is_null()&&folded.find("без зоны")==std::string::npos)
                    return setup_document::rejection("В какой зоне добавить устройство? Укажите её название или явно напишите без зоны.").dump();
                std::size_t begin=0;
                while(begin<folded.size()) {
                    const auto end=folded.find_first_of(".\n;!?,",begin);
                    const auto clause=folded.substr(begin,end==std::string::npos?end:end-begin);
                    if((clause.find("не добавляй")!=std::string::npos||clause.find("не создавай")!=std::string::npos) && setup_document::mentioned(clause,name))
                        throw std::invalid_argument("Creation excluded by the final request");
                    if(end==std::string::npos)break;
                    begin=end+1;
                }
            }
        }
        if(!distinct.insert(op.dump()).second)continue; // Exact repeats only, never merge same names across zones.
        operations.push_back(op);
    }
    // The original document remains the evidence, including unsupported tails.
    const auto transportMention=[&](const std::string& protocol) {return folded.find("транспорт "+protocol)!=std::string::npos||folded.find("протокол "+protocol)!=std::string::npos||folded.find("по "+protocol)!=std::string::npos||folded.find(protocol+" подключ")!=std::string::npos||folded.find(protocol+" соедин")!=std::string::npos;};
    const bool rawTcp=transportMention("tcp")&&folded.find("modbus")==std::string::npos;
    if(rawTcp||transportMention("udp")||transportMention("can"))
        return setup_document::rejection("Указанный транспорт не поддержан этим планом; уточните весь запрос без него.").dump();
    const auto proposal=Json{{"status","ok"},{"summary","Подготовлен полный план. Проверьте объекты, возможности и подключения перед применением."},{"operations",operations}};
    auto result=Json::parse(decodeSetupCopilotProposal(proposal.dump(),catalog));
    if(result.contains("configuration")) {
        // Preserve the existing full-catalog target guard for newly created
        // devices while allowing a complete multi-room document.
        const auto resolved=runtime::DomainView(catalog).resolve(input,false);
        std::set<std::string> allowedRooms(resolved.locations.begin(),resolved.locations.end());
        for(const auto& room:original.at("rooms"))if(setup_document::mentioned(input,room.at("name")))allowedRooms.insert(room.at("id"));
        if(allowedRooms.empty()&&!contextLocation.empty())allowedRooms.insert(contextLocation);
        if(allowedRooms.empty()) {
            for(const auto& room:original.at("rooms"))if(std::find(focus.begin(),focus.end(),room.at("id"))!=focus.end())allowedRooms.insert(room.at("id"));
            if(allowedRooms.size()>1)allowedRooms.clear();
            else if(allowedRooms.empty()&&original.at("rooms").size()==1)allowedRooms.insert(original.at("rooms")[0].at("id"));
        }
        for(const auto& room:result.at("configuration").at("rooms"))if(!catalog->roomIndex().count(room.at("id").get<std::string>()))allowedRooms.insert(room.at("id"));
        for(const auto& device:result.at("configuration").at("devices"))if(!catalog->deviceIndex().count(device.at("id").get<std::string>()) &&
            !device.at("room_id").is_null()&&!allowedRooms.count(device.at("room_id").get<std::string>()))
            return setup_document::rejection("В какой зоне добавить устройство? Укажите её название.").dump();
        if(folded.find("не добавляй")!=std::string::npos&&folded.find("удал")==std::string::npos) {
            for(const auto& old:original.at("devices")) {
                const bool exists=std::any_of(result.at("configuration").at("devices").begin(),result.at("configuration").at("devices").end(),[&](const Json& d){return d.at("id")==old.at("id");});
                if(!exists&&std::find(focus.begin(),focus.end(),old.at("id"))==focus.end())throw std::invalid_argument("Only an uncommitted device creation can be cancelled");
            }
        }
        setup_document::requireExplicitCoverage(result.at("configuration"),input);
        result["proposal_scope"]="configuration";
    }
    return result.dump();
}
}
