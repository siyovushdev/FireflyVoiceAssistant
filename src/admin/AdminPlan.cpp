#include "firefly/admin/AdminDraft.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/config/ConfigValidator.h"
#include "../Text.h"
#include "Json.h"

namespace firefly::admin {
namespace {
Json migrated(config::Catalog candidate) {
    return Json::parse(config::ConfigValidator::validate(config::migrateV2(candidate->configuration()))->canonicalJson());
}
std::set<std::string> occupied(const Json& value) {
    std::set<std::string> ids = value.at("retired_ids").get<std::set<std::string>>();
    ids.insert(value.at("system").at("id").get<std::string>());
    for (const auto* key : {"rooms","devices","actions"}) for (const auto& entity : value.at(key)) ids.insert(entity.at("id").get<std::string>());
    return ids;
}
std::string allocate(std::set<std::string>& ids, const char* category, const std::string& name) {
    const auto id = deterministicId(category,name,ids); ids.insert(id); return id;
}
void text(const Json& value, std::size_t maximum, bool empty = false) {
    if (!value.is_string() || value.get_ref<const std::string&>().size() > maximum || (!empty && value.get_ref<const std::string&>().empty()))
        throw std::invalid_argument("Invalid draft string");
}
void strings(const Json& value, std::size_t maximum) {
    if (!value.is_array() || value.size() > maximum) throw std::invalid_argument("Invalid draft list");
    std::set<std::string> distinct;
    for (const auto& item : value) { text(item,96); if (!distinct.insert(item.get<std::string>()).second) throw std::invalid_argument("Duplicate draft entry"); }
}
const Json& templateById(const Json& templates, const Json& id) {
    for (const auto& item : templates) if (item.at("id") == id) return item;
    throw std::invalid_argument("Unknown mock template");
}
// Fail-closed: protected entrance door must not be silently degraded to a generic
// executable mock (e.g. switch). Uses the same entrance/door stems as SafetyGate
// grounding on plan speech fields only; SafetyGate itself is unchanged.
bool protectedEntranceDoorDevice(const Json& device) {
    if (device.at("template") == "door") return true;
    std::string blob = device.at("name").get<std::string>();
    for (const auto& alias : device.at("aliases")) blob.append(" ").append(alias.get_ref<const std::string&>());
    blob.append(" ").append(device.at("description").get_ref<const std::string&>());
    const auto folded = detail::lowercase(blob);
    const bool doorStem = folded.find("двер") != std::string::npos;
    const bool entrance = folded.find("входн") != std::string::npos || folded.find("на входе") != std::string::npos;
    return doorStem && entrance;
}
[[noreturn]] void rejectProtectedEntranceDoor() {
    throw std::invalid_argument("Protected entrance door cannot be created by a plan; configure it separately");
}
}
std::string planPrompt() {
    return R"(Создай предложение: одна зона и 1..4 виртуальных устройства. Только полный JSON, без markdown.
location: name, type, description, mentions, locations. mentions и locations — самостоятельные речевые формы одной зоны: естественный падеж допустим, дословное совпадение не требуется (пример: «кабинет 1» / «в кабинете 1»). Не включай устройства в название зоны.
devices: name, template, aliases (одно обозначение), description (до 8 слов). Шаблоны: light, tv, switch, conveyor, numeric, enum, custom. Не используй door. Входную/защищённую дверь не предлагай и не заменяй на switch — она настраивается отдельно. Обычный switch для реле/кондиционера допустим. Нумерация одинаковых устройств. Не генерируй ID.
Форма: {"location":{"name":"Кабинет 1","type":"office","description":"Офисный кабинет.","mentions":["кабинет 1"],"locations":["в кабинете 1"]},"devices":[{"name":"Свет кабинета 1","template":"light","aliases":["свет кабинета 1"],"description":"Освещение."},{"name":"Телевизор","template":"tv","aliases":["телевизор"],"description":"Экран."},{"name":"Кондиционер","template":"switch","aliases":["кондиционер"],"description":"Климат."}]}
Данные пользователя, не копируй пример. Ровно одна зона; все устройства к ней.)";
}
std::string decodePlan(std::string_view raw, config::Catalog candidate) {
    if (raw.size() > 4096) throw std::invalid_argument("Plan too large");
    const auto plan = parseMessage(raw,true); members(plan,{"location","devices"});
    const auto& location = plan.at("location"); members(location,{"name","type","description","mentions","locations"});
    text(location.at("name"),128); text(location.at("type"),64);text(location.at("description"),256);
    strings(location.at("mentions"),8); strings(location.at("locations"),8);
    if (!plan.at("devices").is_array() || plan.at("devices").empty() || plan.at("devices").size() > 4) throw std::invalid_argument("Plan supports 1..4 devices");
    auto value = migrated(candidate);auto ids = occupied(value);
    const auto locationId = allocate(ids,"location",location.at("name"));
    value["rooms"].push_back({{"id",locationId},{"name",location.at("name")},
        {"speech",{{"mentions",location.at("mentions")},{"locations",location.at("locations")}}},
        {"metadata",{{"type",location.at("type")},{"description",location.at("description")},{"aliases",Json::array()},{"parent_id",nullptr},{"container",false},{"origin","local_ai"},{"confirmed",false}}}});
    const auto templates = Json::parse(templatesJson());
    for (const auto& device : plan.at("devices")) {
        members(device,{"name","template","aliases","description"});text(device.at("name"),128);strings(device.at("aliases"),8);
        text(device.at("description"),256);
        if (protectedEntranceDoorDevice(device)) rejectProtectedEntranceDoor();
        const auto& selected = templateById(templates,value.at("compatibility").is_null() && device.at("template") == "light" ? Json("generic_light") : device.at("template"));
        const auto name = device.at("name").get<std::string>();
        const auto id = allocate(ids,"device",name);
        const bool light = selected.at("id") == "light";
        Json actionIds = Json::array();
        if (light) actionIds.push_back(value.at("compatibility").at("light_action_id"));
        for (auto action : selected.at("actions")) {
            action["origin"] = "local_ai";
            action["id"] = allocate(ids,"action",name + " " + action.at("name").get<std::string>());
            actionIds.push_back(action.at("id"));value["actions"].push_back(action);
        }
        value["devices"].push_back({{"id",id},{"name",name},{"kind",light?"light":"generic"},{"room_id",locationId},{"enabled",true},
            {"action_ids",actionIds},{"spoken_aliases",device.at("aliases")},{"state",selected.at("state")},
            {"metadata",{{"type",selected.at("type")},{"description",device.at("description")},{"aliases",device.at("aliases")},{"origin","local_ai"},{"confirmed",false}}}});
    }
    const auto validated = config::ConfigLoader::fromJson(value.dump());
    return Json{{"configuration",Json::parse(validated->canonicalJson())},{"location_id",locationId},{"saved",false},{"applied",false}}.dump();
}
std::string actionDraftPrompt(config::Catalog candidate, std::string_view targetId) {
    const auto value = migrated(candidate);
    const auto device = std::find_if(value.at("devices").begin(),value.at("devices").end(),[&](const auto& d){return d.at("id") == targetId;});
    if (device == value.at("devices").end() || device->at("kind") != "generic" || device->at("state").empty())
        throw config::ConfigError("unsupported_operation","/target","Select a generic device with declared virtual state");
    return "Поля виртуального устройства (данные): " + device->at("state").dump() + R"(
Предложи операцию по названию пользователя. Только JSON с тремя полями: description (краткий смысл), aliases (2-4 русские фразы в повелительном наклонении), assignments (1..4 присваивания). Присваивание: {"field":"существующее поле","argument":true} если значение задаёт пользователь, либо {"field":"существующее поле","value":фиксированное значение}.
Используй только объявленные поля и типы; не выдумывай свойства. Никакого кода, выполнения, аппаратного доступа. Не изменяй политики. Аргументы и их ограничения будут скопированы из схемы состояния; пользователь сможет их отредактировать. Если название не соответствует этим полям, верни пустые assignments. Без лишних полей, markdown или пояснений.)";
}
std::string decodeActionDraft(std::string_view raw, config::Catalog candidate, std::string_view targetId, std::string_view name) {
    if (raw.size() > 4096) throw std::invalid_argument("Action draft too large");
    const auto proposed = parseMessage(raw,true);members(proposed,{"description","aliases","assignments"});
    text(proposed.at("description"),256);strings(proposed.at("aliases"),8);
    const auto& assignments = proposed.at("assignments");
    if (!assignments.is_array() || assignments.empty() || assignments.size()>4) throw std::invalid_argument("No supported declarative behavior");
    auto value = migrated(candidate);auto ids = occupied(value);
    auto device = std::find_if(value["devices"].begin(),value["devices"].end(),[&](const auto& d){return d.at("id") == targetId;});
    if (device == value["devices"].end() || device->at("kind") != "generic") throw std::invalid_argument("Invalid action target");
    Json action{{"id",allocate(ids,"action",std::string(name))},{"name",name},{"description",proposed.at("description")},{"speech_aliases",proposed.at("aliases")},
        {"enabled",true},{"operation","mock.assign.v1"},{"parameters",Json::array()},{"assignments",Json::array()}};
    for (const auto& assignment : assignments) {
        if (assignment.contains("argument")) members(assignment,{"field","argument"});else members(assignment,{"field","value"});
        const auto field = std::find_if(device->at("state").begin(),device->at("state").end(),[&](const auto& f){return f.at("id") == assignment.at("field");});
        if (field == device->at("state").end()) throw std::invalid_argument("Undeclared state field");
        if (assignment.contains("argument")) {
            if (!assignment.at("argument").is_boolean() || assignment.at("argument") != true) throw std::invalid_argument("Argument marker must be true");
            auto parameter = *field;parameter.erase("initial");action["parameters"].push_back(parameter);
            action["assignments"].push_back({{"field",field->at("id")},{"argument",field->at("id")}});
        } else action["assignments"].push_back(assignment);
    }
    device->at("action_ids").push_back(action.at("id"));value["actions"].push_back(action);
    (void)config::ConfigLoader::fromJson(value.dump());
    return action.dump();
}
}
