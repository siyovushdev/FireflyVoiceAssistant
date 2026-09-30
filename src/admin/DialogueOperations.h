#pragma once
#include "Json.h"

namespace firefly::admin {
// Strict, small wire format for setup only. Expand trusted fixed fields into
// existing AdminCopilot operations; that decoder remains authoritative for IDs,
// references, configuration semantics and protected legacy operations.
inline Json expandDialogueOperations(std::string_view raw) {
    if(raw.size()>4096)throw std::invalid_argument("Dialogue result too large");
    const auto wire=parseMessage(raw,true);
    for(const auto* status:{"question","unsupported"})if(wire.contains(status)) {
        members(wire,{status});
        if(!wire.at(status).is_string() || wire.at(status).get_ref<const std::string&>().empty() || wire.at(status).get_ref<const std::string&>().size()>320)
            throw std::invalid_argument("Invalid dialogue question");
        const auto explanation=wire.at(status).get<std::string>();
        if(explanation=="короткий вопрос" || explanation=="конкретный вопрос пользователю" || explanation=="причина" || explanation=="объяснение" || explanation=="...")
            throw std::invalid_argument("Schema placeholder is not a proposal: return actual requested operations or a specific clarification");
        return {{"status",std::string(status)=="question"?"ambiguous":"unsupported"},{"summary",wire.at(status)},{"operations",Json::array()}};
    }
    members(wire,{"ops"});
    if(!wire.at("ops").is_array() || wire.at("ops").empty() || wire.at("ops").size()>3)
        throw std::invalid_argument("Dialogue requires 1..3 operations");
    Json operations=Json::array();
    const std::map<std::string,std::vector<std::string>> fields={
        {"location",{"name"}}, {"power_device",{"location","name"}},
        {"rename_device",{"location","device","name"}},
        {"rename_location",{"location","name","keep_previous_as_alias"}},
        {"delete_device",{"location","device"}},
        {"add_brightness",{"location","device"}},
        {"bounds",{"location","device","field","minimum","maximum"}}
    };
    for(const auto& tuple:wire.at("ops")) {
        if(!tuple.is_array() || tuple.empty() || !tuple[0].is_string())throw std::invalid_argument("Typed operation array required");
        const auto kind=tuple[0].get<std::string>();const auto schema=fields.find(kind);
        if(schema==fields.end())throw std::invalid_argument("Unknown operation; use one of the listed operation names exactly");
        if(tuple.size()!=schema->second.size()+1) {
            std::string expected;for(const auto& field:schema->second)expected+=(expected.empty()?"":",")+field;
            throw std::invalid_argument(kind+" requires ordered arguments ["+expected+"], got "+std::to_string(tuple.size()-1));
        }
        Json operation={{"op",kind=="location"?"create_location":kind=="power_device"?"create_device":kind=="bounds"?"set_field_bounds":kind}};
        for(std::size_t i=0;i<schema->second.size();++i) {
            const auto& field=schema->second[i];const auto& value=tuple[i+1];
            if(field=="minimum"||field=="maximum") {if(!value.is_number())throw std::invalid_argument("Numeric bound required");}
            else if(field=="keep_previous_as_alias") {if(!value.is_boolean())throw std::invalid_argument("Boolean alias policy required");}
            else if(!value.is_string() || value.get_ref<const std::string&>().empty() || value.get_ref<const std::string&>().size()>128)
                throw std::invalid_argument("Bounded entity name required");
            operation[field]=value;
        }
        if(kind=="location"){operation["type"]="room";operation["description"]=operation.at("name");}
        if(kind=="power_device"){operation["template"]="power_only";operation["description"]=operation.at("name");}
        operations.push_back(operation);
    }
    return {{"status","ok"},{"summary","Подготовлены изменения. Проверьте устройства и действия в плане."},{"operations",operations}};
}
}
