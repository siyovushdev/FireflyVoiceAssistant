#pragma once
#include "DialogueOperations.h"
#include "firefly/admin/AdminCopilot.h"
#include "firefly/admin/AdminRequestPolicy.h"
#include "firefly/runtime/DomainView.h"
#include "../Text.h"
#include <regex>

namespace firefly::admin {
// Runs before private draft persistence as well as before any inference.
inline void validateSetupDocumentInput(std::string_view text) {
    validateMessage(text);
    // Permit an explicit decision to enter credentials in their separate form.
    // Only complete, value-free clauses are removed; actual secret assignments
    // remain rejected before inference, history or checkpoint persistence.
    const auto folded = std::regex_replace(detail::lowercase(text),
        std::regex(R"((без пароля|без паролей|пароль введу отдельно|пароли введу отдельно|no password|without password)([ \t]*[.,;!?]|$))"), "$2");
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
        const bool left = at == 0 || !word(static_cast<unsigned char>(value.front())) || !word(static_cast<unsigned char>(input[at - 1]));
        const auto end = at + value.size();
        const bool right = end == input.size() || !word(static_cast<unsigned char>(value.back())) || !word(static_cast<unsigned char>(input[end]));
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
    bool positive=false,negative=false;std::set<std::string> previousTargets;
    for(const auto& clause:clauses(input)) {
        std::set<std::string> targets;
        for(const auto& candidate:names)if(mentioned(clause,candidate))targets.insert(candidate);
        const bool continuation=targets.empty()&&previousTargets.count(name)&&
            (named(clause,"только")||named(clause,"он")||named(clause,"она")||named(clause,"оно")||named(clause,"они")||named(clause,"его")||named(clause,"их"));
        if(!targets.empty())previousTargets=targets;
        const bool collective=named(clause,"оба")||named(clause,"все")||named(clause,"всех")||named(clause,"всем")||named(clause,"обоих")||named(clause,"обоим")||named(clause,"каждое")||clause.find("эти устройства")!=std::string::npos;
        if(!mentioned(clause,name)&&!collective&&!continuation)continue;
        negative=negative||clause.find("без управлен")!=std::string::npos||clause.find("без действ")!=std::string::npos||clause.find("не управл")!=std::string::npos||clause.find("не включ")!=std::string::npos||clause.find("не выключ")!=std::string::npos;
        positive=positive||powerPair(clause);
    }
    return positive&&!negative;
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
        const std::map<std::string,std::string> labels={{"address","address|адрес регистра|адрес катушки"},{"unit_id","unit_id|unit id|slave_id"},
            {"port","port|порт"},{"function","function|fc"},{"baud_rate","baud_rate|baud|бод"},{"stop_bits","stop_bits"}};
        const auto label=labels.find(field);
        if(label!=labels.end()) {
            const auto text=detail::lowercase(input);const std::regex pattern("(?:^|[^a-z0-9_])(?:"+label->second+")[ \\t\"':=]*([0-9]+)");
            bool present=false,matched=false;
            for(auto it=std::sregex_iterator(text.begin(),text.end(),pattern);it!=std::sregex_iterator();++it) {present=true;matched=matched||it->str(1)==value.dump();}
            if(present&&!matched)throw std::invalid_argument("unstated_connection_fact: "+key+" differs from the explicitly labelled value");
        }
    }
    if (value.is_string() && literal(input, value.get<std::string>())) return;
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
    const std::string text(input),folded=detail::lowercase(input);
    const auto values=[&](const char* category,const char* protocol,const char* key) {
        std::set<std::string> out;
        for(const auto& entry:candidate.value(category,Json::array())) if(entry.contains(protocol)&&entry.at(protocol).contains(key)) {
            const auto& value=entry.at(protocol).at(key);out.insert(value.is_string()?value.get<std::string>():value.dump());
        }
        return out;
    };
    if(folded.find("mqtt")!=std::string::npos) {
        auto topics=values("bindings","mqtt","topic");const auto observations=values("observations","mqtt","topic");topics.insert(observations.begin(),observations.end());
        const std::regex topic(R"((?:topic|топик|состояние)[^\n.;]{0,30}?\s([A-Za-z0-9_-]+(?:/[A-Za-z0-9_.-]+)+))");
        for(auto it=std::sregex_iterator(folded.begin(),folded.end(),topic);it!=std::sregex_iterator();++it) {
            const auto declared=text.substr(static_cast<std::size_t>(it->position(1)),static_cast<std::size_t>(it->length(1)));
            if(!topics.count(declared))throw std::invalid_argument("Incomplete plan: an explicitly stated MQTT topic has no mapping or observation");
        }
        const std::regex payload(R"((?:^|[^A-Za-z0-9_])payload[ "']*[:=]?[ "']*([A-Za-z0-9_-]+))");
        const auto sent=values("bindings","mqtt","payload");
        for(auto it=std::sregex_iterator(folded.begin(),folded.end(),payload);it!=std::sregex_iterator();++it) {
            const auto declared=text.substr(static_cast<std::size_t>(it->position(1)),static_cast<std::size_t>(it->length(1)));
            if(!sent.count(declared))throw std::invalid_argument("Incomplete plan: explicitly stated MQTT payload is missing");
        }
    }
    if(folded.find("http")!=std::string::npos) {
        const std::regex route(R"((post|put|get)[ \t]+(/[A-Za-z0-9_./?=&%-]+))");
        for(auto it=std::sregex_iterator(folded.begin(),folded.end(),route);it!=std::sregex_iterator();++it) {
            auto method=it->str(1);std::transform(method.begin(),method.end(),method.begin(),[](unsigned char c){return std::toupper(c);});
            const auto path=text.substr(static_cast<std::size_t>(it->position(2)),static_cast<std::size_t>(it->length(2)));bool found=false;
            for(const auto& entry:candidate.value(method=="GET"?"observations":"bindings",Json::array()))
                if(entry.contains("http")&&entry.at("http").value("method",std::string())==method&&entry.at("http").value("path",std::string())==path)found=true;
            if(!found)throw std::invalid_argument("Incomplete plan: explicitly stated HTTP method/path is missing");
        }
    }
    if(folded.find("modbus")!=std::string::npos) {
        auto addresses=values("bindings","modbus","address");const auto observations=values("observations","modbus","address");addresses.insert(observations.begin(),observations.end());
        const std::regex address(R"((?:address|адрес регистра|адрес катушки)[ \t:=]+([0-9]+))");
        for(auto it=std::sregex_iterator(folded.begin(),folded.end(),address);it!=std::sregex_iterator();++it)
            if(!addresses.count(it->str(1)))throw std::invalid_argument("Incomplete plan: explicitly stated Modbus address is missing");
        const auto written=values("bindings","modbus","value");
        const std::regex writeValue(R"((?:^|[^A-Za-z0-9_])value[ \t:=]+(true|false|-?[0-9]+)(?:$|[^A-Za-z0-9_]))");
        for(auto it=std::sregex_iterator(folded.begin(),folded.end(),writeValue);it!=std::sregex_iterator();++it)
            if(!written.count(it->str(1)))throw std::invalid_argument("Incomplete plan: explicitly stated Modbus write value is missing");
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
        prompt+=R"(
Transport values must come exactly from INPUT_DATA or the selected existing CATALOG entry. Missing endpoint/topic/payload/address/serial facts require a question, NOT a semantic-only partial plan. No invented credentials_id. Safe application defaults: timeout_ms=2000, qos=0, retain=false, tls=false, HTTP status=200. Connection arguments refer to the connection NAME, never to a topic or device name. Include all specified writes and observations.)";
        if(mqtt)prompt+=R"(
MQTT operations (flat JSON arrays):
["mqtt_connection",connection_name,host_string,port_number,tls_boolean,credentials_id_string_or_null]
["mqtt_binding",zone_name,device_name,action_name,connection_name,topic_string,payload_string,qos_number,retain_boolean]
["mqtt_observe",zone_name,device_name,connection_name,topic_string,state_field,true_payload_string,false_payload_string,qos_number]
Use action_name "ON" for turning on, "OFF" for turning off; one binding per action. Copy payload bytes exactly. Observation is format text; true_payload and false_payload are STRINGS, not booleans. When input says true_payload ON and false_payload OFF, use "ON","OFF". Do not invent credentials: use null when no credentials_id was provided. JSON observations require the manual editor.)";
        if(http)prompt+=R"(
HTTP operations:
["http_connection",connection_name,base_url_string,base_path_string] (base_path defaults to "")
["http_binding",zone_name,device_name,action_name,connection_name,method_string,path_string,body_object,status_number]
["http_observe",zone_name,device_name,connection_name,path_string,fields_object,status_number] uses GET.
Only body and fields are JSON objects: body example {"power":true}, fields example {"on":"power"}. Copy the user's exact keys, values, URL and paths. action_name is "ON" or "OFF" or an existing action name. Command methods POST/PUT. status defaults to 200.
Syntax example ONLY for existing objects (replace all example values with the user's data): {"complete":true,"ops":[["http_binding","Zone","Relay","ON","Web","POST","/set",{"power":true},200],["http_observe","Zone","Relay","Web","/status",{"power":"power"},200]]}
Every operation closes with ], never ). An object inside an operation closes with }, then the operation continues until ].)";
        if(modbus)prompt+=R"(
Modbus operations (flat JSON arrays):
["modbus_tcp_connection",connection_name,host_string,port_number,unit_id_number]
["modbus_rtu_connection",connection_name,serial_port_string,baud_rate_number,parity_string,stop_bits_number,unit_id_number]
["modbus_binding",zone_name,device_name,action_name,connection_name,function_number,address_number,data_type_string,value]
["modbus_observe",zone_name,device_name,connection_name,function_number,address_number,data_type_string,state_field]
Use action_name "ON" or "OFF", with a separate binding for each. Write functions 5/6, read 1/2/3/4. Types boolean/uint16/int16. Every numeric transport field must be supplied by the user. unit_id selects the slave in the CONNECTION; address selects its coil/register in the BINDING. Do not confuse them. Copy every digit, including zeros. Boolean value is true/false, not a string. State field for power is "power".
Positional example ONLY, not user data: if unit_id=7 and address=1234, a write is ["modbus_binding","Zone","Relay","ON","Bus",5,1234,"boolean",true] and a read is ["modbus_observe","Zone","Relay","Bus",1,1234,"boolean","power"]. Neither contains 7. After the last operation close its array, the ops array, then the outer object: ]]} . Do not add a second }.)";
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
    for(auto& operation:wire["ops"])operation=setup_document::transportTuple(operation);
    const bool connectionPlan=std::any_of(wire.at("ops").begin(),wire.at("ops").end(),[](const Json& op){return op.is_array()&&!op.empty()&&op[0].is_string()&&(op[0]=="connection"||op[0]=="binding"||op[0]=="observe");});
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
        std::set<std::string> targets;for(const auto& name:deviceNames)if(setup_document::mentioned(clause,name))targets.insert(name);
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
                if(!setup_document::named(input,name)&&prior.is_null())throw std::invalid_argument("Connection name was not supplied");
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
