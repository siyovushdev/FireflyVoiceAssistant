#pragma once
#include "AdminSetupDocument.h"
#include <chrono>
#include <functional>
#include <sstream>

namespace firefly::admin {
inline bool stagedSetupDocument(std::string_view input) {
    const auto text=detail::lowercase(input);
    return input.size()>1800 && (text.find("добав")!=std::string::npos||text.find("созда")!=std::string::npos) &&
        (setup_document::named(text,"MQTT")||setup_document::named(text,"HTTP")||setup_document::named(text,"Modbus"));
}
using SetupDocumentCall=std::function<Json(const std::string&,std::string_view,int,
    const std::function<std::string(std::string_view)>&,int)>;

// Keep exact bytes/case from explicitly named device sections for transport
// grounding. The complete original request still reaches every model call and
// the final validator; this evidence only prevents borrowing a neighbour's facts.
inline std::string setupDeviceFacts(std::string_view input,const std::string& name,const std::vector<std::string>& names) {
    std::vector<setup_source::Target> targets;for(const auto& n:names)targets.push_back({"",n});
    return setup_source::Document(input,targets).forTarget({"",name});
}
inline int setupConnectionTimeout(std::string_view facts) {
    const auto text=detail::lowercase(facts);
    const auto protectedText=setup_source::protectedValues(facts);
    const std::regex field(R"((?:^|[ \t\r\n;,("'])(timeout_ms|timeout|тайм-аут|тайм аут|таймаут)(?=[ \t"':=—–+0-9-]|$))");
    std::set<int> values;
    bool declared=false;
    for(auto i=std::sregex_iterator(text.begin(),text.end(),field);i!=std::sregex_iterator();++i) {
        if(protectedText[static_cast<std::size_t>(i->position(1))])
            throw std::invalid_argument("Уточните тайм-аут подключения отдельным полем; timeout внутри JSON или строкового значения неоднозначен.");
        declared=true;
        const auto start=static_cast<std::size_t>(i->position()+i->length());
        const auto tail=text.substr(start);std::smatch number;
        if(!std::regex_search(tail,number,std::regex(R"(^[ \t\r\n"':=—–]*([0-9]+))")))
            throw std::invalid_argument("Укажите каждый тайм-аут положительным целым числом миллисекунд.");
        const auto end=start+static_cast<std::size_t>(number.length());
        auto suffix=text.substr(end,text.find_first_of("\n.;",end)-end);
        const bool hasUnit=suffix.find_first_not_of(" \t\r")!=std::string::npos;
        if((end+1<text.size()&&(text[end]=='.'||text[end]==',')&&std::isdigit(static_cast<unsigned char>(text[end+1])))||
           (hasUnit&&!std::regex_search(suffix,std::regex(R"(^\s*(мс|ms|миллисекунд(?:а|ы)?)(?:\s|$))"))))
            throw std::invalid_argument("Укажите тайм-аут этого устройства в миллисекундах.");
        const int n=std::stoi(number.str(1));if(n<=0)throw std::invalid_argument("Positive timeout required");values.insert(n);
    }
    if(!declared)return 2000;
    if(values.size()!=1)throw std::invalid_argument("Укажите один явный тайм-аут устройства в миллисекундах.");
    return *values.begin();
}

// Each stage sees the entire original document. Intermediate objects are local
// values only: the ordinary complete-document decoder admits the combined plan.
inline Json planSetupDocument(config::Catalog catalog,std::string_view input,const Json& focus,
                              const std::string& contextLocation,const SetupDocumentCall& call,
                              const std::function<std::chrono::steady_clock::time_point()>& now=[] {return std::chrono::steady_clock::now();}) {
    validateSetupDocumentInput(input);
    const auto begin=now();
    Json trace=Json::array();int retries=1;
    const auto fail=[&](const std::string& reason) {
        return Json{{"ok",false},{"saved",false},{"applied",false},{"trace",trace},
            {"retry_count",1-retries},{"retry_limit",1},{"provenance","ai_suggested"},
            {"errors",failure("invalid_ai_response","",reason)["errors"]}};
    };
    const auto ask=[&](const std::string& group,const std::string& prompt,const std::function<std::string(std::string_view)>& decode) {
        // 384 tokens uses the existing 60-second context/inference watchdog.
        // Reserve both context+generation and the one shared format retry.
        const auto elapsed=now()-begin;
        const int offeredRetries=retries&&elapsed+std::chrono::seconds(240)<=std::chrono::seconds(570)?1:0;
        if(elapsed+std::chrono::seconds(120)>std::chrono::seconds(570))
            return fail("Описание слишком большое для одного сеанса. Настройки не изменены; разделите запрос.");
        auto result=call(prompt,input,384,decode,offeredRetries);
        const int used=result.value("retry_count",0);
        if(used<0||used>offeredRetries)throw std::runtime_error("Invalid shared setup retry accounting");
        retries-=used;
        for(auto entry:result.value("trace",Json::array())) {entry["group"]=group;trace.push_back(std::move(entry));}
        result["trace"]=trace;result["retry_count"]=1-retries;result["retry_limit"]=1;
        return result;
    };
    const auto existing=Json::parse(catalog->canonicalJson());
    const std::string inventoryPrompt=R"(Read ALL of INPUT_DATA.text. First list ONLY the NEW devices explicitly requested, with their zones and transport. Do not configure or execute anything. Preserve existing CATALOG objects. Respect exclusions and final restrictions. Return one valid JSON object:
{"devices":[["exact zone name","exact new device name","mqtt"]]}
Use one array per new device. transport is mqtt/http/modbus_tcp/modbus_rtu. Use the user's exact names. Include every requested new device exactly once, maximum 10. Never list existing devices as new. All devices in this flow must explicitly have ON/OFF. If something is missing or the request needs other changes, return only {"question":"short specific Russian question"}. Unsupported work: {"unsupported":"short Russian reason"}. No commentary, endpoints, invented objects or partial lists.
CATALOG=)"+setup_document::compactCatalog(catalog).dump();
    auto inventory=ask("inventory",inventoryPrompt,[&](std::string_view raw) {
        const auto value=parseMessage(raw,true);
        if(value.contains("question")||value.contains("unsupported")) {
            (void)decodeSetupDocument(raw,catalog,input,focus,contextLocation);return value.dump();
        }
        members(value,{"devices"});
        if(!value.at("devices").is_array()||value.at("devices").empty()||value.at("devices").size()>10)
            throw std::invalid_argument("List 1..10 new devices, never a partial list");
        std::set<std::string> seen;
        for(const auto& d:value.at("devices")) {
            if(!d.is_array()||d.size()!=3||!d[0].is_string()||!d[1].is_string()||!d[2].is_string())
                throw std::invalid_argument("Expected zone, device, transport");
            const auto zone=d[0].get<std::string>(),name=d[1].get<std::string>(),type=d[2].get<std::string>();
            if(zone.empty()||name.empty()||!setup_document::mentioned(input,zone)||!setup_document::mentioned(input,name)||
               !std::set<std::string>{"mqtt","http","modbus_tcp","modbus_rtu"}.count(type)||!seen.insert(Json::array({zone,name}).dump()).second)
                throw std::invalid_argument("Inventory contains an unstated or duplicate device");
            for(const auto& old:existing.at("devices"))if(detail::lowercase(old.at("name").get<std::string>())==detail::lowercase(name))
                for(const auto& room:existing.at("rooms"))if(old.at("room_id")==room.at("id")&&detail::lowercase(room.at("name").get<std::string>())==detail::lowercase(zone))
                    throw std::invalid_argument("Inventory cannot replace an existing device");
        }
        std::vector<setup_source::Target> declared;std::vector<std::string> zones;
        for(const auto& d:value.at("devices"))declared.push_back({d[0],d[1]});
        for(const auto& room:existing.at("rooms"))zones.push_back(room.at("name"));
        setup_source::Document(input,declared,zones).requireInventory(declared);
        return value.dump();
    });
    if(!inventory.value("ok",false))return inventory;
    const auto list=inventory.at("suggestion");
    if(!list.contains("devices")) {inventory["suggestion"]=Json::parse(decodeSetupDocument(list.dump(),catalog,input,focus,contextLocation));return inventory;}
    Json ops=Json::array();
    std::vector<setup_source::Target> requested;std::vector<std::string> zones;
    for(const auto& d:list.at("devices"))requested.push_back({d[0],d[1]});
    for(const auto& room:existing.at("rooms"))zones.push_back(room.at("name"));
    const setup_source::Document document(input,requested,zones);
    for(const auto& d:list.at("devices")) {
        const auto type=d[2].get<std::string>();
        const auto deviceFacts=document.forTarget({d[0],d[1]});
        std::string prompt=R"(Read ALL of INPUT_DATA.text, including restrictions. Configure ONLY TARGET, one new ON/OFF device. Other devices are handled separately. Return ONE JSON object with EXACTLY THREE keys: connection,commands,observation. The server already knows TARGET's name and zone: do NOT repeat op, zone, device, ops or complete fields. No other devices.
connection is the array below for a NEW connection, or an exact existing CATALOG connection name string only when its protocol and endpoint match. Never change existing settings. New connection naming is automatic. Every endpoint, command, payload, address and serial parameter must come from this device's description; never borrow another device's values. Missing facts: return {"question":"specific short Russian question"} instead. No execution.
observation MUST be null when no independent readback was provided. Otherwise use the observation array below. JSON bodies are objects, not strings. Keep payload case, numeric zeros, and the stated timeout. Defaults only: timeout=2000, qos=0, retain=false, tls=false, credentials_id=null, base_path="", HTTP status=200.
)";
        if(type=="mqtt")prompt+=R"(connection=["mqtt",host,port,tls,credentials_id,timeout]
commands=["mqtt",on_topic,on_payload,off_topic,off_payload,qos,retain]
observation=["mqtt",state_topic,on_payload,off_payload,qos] or null. Payloads are exact strings.
For a NEW connection credentials_id MUST be null unless INPUT_DATA.text explicitly supplies the exact credentials_id value. Never copy credentials_id from another CATALOG connection into a new one. Reusing existing credentials means referencing that whole matching connection by its name, not creating another connection with borrowed credentials.
If one QoS is given for this device, use it for both commands and observation unless a separate readback QoS is explicitly stated.
)";
        else if(type=="http")prompt+=R"(connection=["http",base_url,base_path,timeout]
base_url MUST include the explicitly supplied port, for example the URI syntax http://HOST:PORT. base_path is empty unless explicitly specified. A command path is NOT a base_path.
commands=["http",method,on_path,on_body_object,off_path,off_body_object,status]
observation=["http",GET_path,fields_object,status] or null. If readback is unknown, use null, never invent it.
)";
        else {
            prompt+=type=="modbus_tcp"?"connection=[\"modbus_tcp\",host,port,unit_id,timeout]\n":"connection=[\"modbus_rtu\",serial_port,baud_rate,parity,stop_bits,unit_id,timeout]\nRTU parity none/even/odd. One stop bit is 1. Eight data bits are fixed and NOT another array element.\n";
            prompt+=R"(commands=["modbus",write_function,address,data_type,on_value,off_value]
observation=["modbus",read_function,address,data_type] or null.
For coil functions 1/2/5 data_type is "boolean", values true/false. For register functions 3/4/6 require explicit uint16/int16. Copy address exactly including 0. unit_id and address are different fields, never borrow another device's values.
)";
        }
        prompt+="CATALOG="+setup_document::compactCatalog(catalog).dump()+"\nTARGET="+d.dump()+
            "\nTARGET_DETAILS="+Json(deviceFacts).dump()+
            "\nReturn ONLY this TARGET's connection,commands,observation. TARGET_DETAILS quotes its original lines. Never take another device's endpoint. HTTP bodies are objects: {\"on\":true}, never quoted strings. Read the complete input for restrictions.";
        auto part=ask("device:"+d[1].get<std::string>(),prompt,[&](std::string_view raw) {
            auto value=parseMessage(raw,true);
            if(value.contains("question")||value.contains("unsupported")) {
                (void)decodeSetupDocument(raw,catalog,input,focus,contextLocation);return value.dump();
            }
            members(value,{"connection","commands","observation"});
            value["op"]="power_setup";value["zone"]=d[0];value["device"]=d[1];
            auto& c=value.at("connection");
            if(c.is_array()&&(c.empty()||c[0]!=type))throw std::invalid_argument("Target transport mismatch");
            // Timeout is an application-owned default or an explicit numeric
            // source field, never a guess imported from another device by AI.
            // Validate tuple shape before replacing this one non-command field.
            (void)setup_document::expandPowerSetups(Json::array({value}),catalog);
            if(c.is_array())c.back()=setupConnectionTimeout(deviceFacts);
            auto expanded=setup_document::expandPowerSetups(Json::array({value}),catalog);
            for(auto& tuple:expanded)tuple=setup_document::transportTuple(tuple);
            // Check facts while the format retry can still correct this stage.
            // These are new-device mappings; no other device's credentials or
            // binding values can serve as their implicit prior configuration.
            Json coverage={{"bindings",Json::array()},{"observations",Json::array()}};
            for(const auto& tuple:expanded) {
                const auto typed=setup_document::transportTuple(tuple);
                if(typed[0]=="connection"||typed[0]=="binding"||typed[0]=="observe")
                    setup_document::facts(typed.back(),Json(),deviceFacts);
                if(typed[0]=="binding")coverage["bindings"].push_back(typed.back());
                if(typed[0]=="observe")coverage["observations"].push_back(typed.back());
            }
            // Missing stated readback/commands must get the shared retry now,
            // not silently disappear into an otherwise well-formed final plan.
            setup_document::requireExplicitCoverage(coverage,deviceFacts);
            setup_document::validatePowerSources(expanded,catalog,input,requested,false);
            return value.dump();
        });
        if(!part.value("ok",false))return part;
        const auto value=part.at("suggestion");
        if(value.contains("question")||value.contains("unsupported")) {part["suggestion"]=Json::parse(decodeSetupDocument(value.dump(),catalog,input,focus,contextLocation));return part;}
        ops.push_back(value);
    }
    try {
        auto result=Json::parse(decodeSetupDocument(Json{{"complete",true},{"ops",ops}}.dump(),catalog,input,focus,contextLocation));
        return Json{{"ok",true},{"saved",false},{"applied",false},{"provenance","ai_suggested"},
            {"suggestion",result},{"trace",trace},{"retry_count",1-retries},{"retry_limit",1}};
    } catch(const std::exception& error) {
        trace.push_back({{"stage","combined_validation"},{"error_code","invalid_ai_response"},{"error",error.what()}});
        return fail("AI не подготовила согласованный общий план. Настройки не изменены; уточните запрос.");
    }
}
}
