#include "firefly/config/ConfigLoader.h"
#include "firefly/config/ConfigValidator.h"
#include "JsonInternal.h"
#include "Platform.h"
#include "ConfigSchema.h"
#include <algorithm>
#include <regex>

namespace firefly::config {
ConfigError::ConfigError(std::string codeValue, std::string pathValue, std::string message)
    : std::runtime_error(std::move(message)), code(std::move(codeValue)), path(std::move(pathValue)) {}
Catalog ConfigLoader::fromJson(std::string_view bytes) {
    const auto document = detail::parseDocument(bytes);
    detail::validateStructure(document);
    return ConfigValidator::validate(detail::decode(document));
}
Catalog ConfigLoader::fromFile(const std::string& path) { return fromJson(detail::readFile(path)); }
}

namespace firefly::config::detail {
namespace {
std::string pointerToken(const std::string& key) {
    std::string result;
    for (const char c : key) result += c == '~' ? "~0" : c == '/' ? "~1" : std::string(1, c);
    return result;
}
bool typeMatches(const Json& value, const std::string& type) {
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "string") return value.is_string();
    if (type == "boolean") return value.is_boolean();
    if (type == "null") return value.is_null();
    if (type == "integer") return value.is_number_integer();
    throw std::logic_error("Unsupported bundled schema type");
}
// Deliberately limited to the vocabulary used by the bundled v1 schema.
// This is not a general-purpose or remotely extensible JSON Schema engine.
void checkSchema(const Json& value, const Json& schema, const Json& root, const std::string& path) {
    const auto fail = [&](const std::string& message) { throw ConfigError("schema_error", path, message); };
    if (schema.contains("$ref")) {
        const auto reference = schema.at("$ref").get<std::string>();
        if (reference.rfind("#/$defs/", 0) != 0) throw std::logic_error("Nonlocal bundled schema reference");
        checkSchema(value, root.at(Json::json_pointer(reference.substr(1))), root, path);
        return;
    }
    if (schema.contains("type")) {
        const auto& types = schema.at("type");
        bool valid = false;
        if (types.is_string()) valid = typeMatches(value, types.get<std::string>());
        else for (const auto& type : types) valid = typeMatches(value, type.get<std::string>()) || valid;
        if (!valid) fail("Wrong JSON type");
    }
    if (schema.contains("const") && value != schema.at("const")) fail("Unsupported constant");
    if (schema.contains("enum") && std::find(schema.at("enum").begin(), schema.at("enum").end(), value) == schema.at("enum").end())
        fail("Unsupported value");
    if (value.is_object()) {
        for (const auto& required : schema.value("required", Json::array())) {
            const auto key = required.get<std::string>();
            if (!value.contains(key)) throw ConfigError("missing_field", path + "/" + pointerToken(key), "Required field missing");
        }
        const auto properties = schema.value("properties", Json::object());
        for (auto item = value.begin(); item != value.end(); ++item) {
            const auto child = path + "/" + pointerToken(item.key());
            if (properties.contains(item.key())) checkSchema(item.value(), properties.at(item.key()), root, child);
            else if (!schema.value("additionalProperties", true)) throw ConfigError("unknown_field", child, "Unknown field");
        }
    }
    if (value.is_array()) {
        if (value.size() < schema.value("minItems", std::size_t{0}) || value.size() > schema.value("maxItems", MaxConfigBytes))
            fail("Array size outside schema bounds");
        if (schema.contains("items")) for (std::size_t i = 0; i < value.size(); ++i)
            checkSchema(value[i], schema.at("items"), root, path + "/" + std::to_string(i));
    }
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        checkText(text, path);
        const auto length = static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; }));
        if (length < schema.value("minLength", std::size_t{0}) || length > schema.value("maxLength", MaxConfigBytes))
            fail("String length outside schema bounds");
        if (schema.contains("pattern")) {
            static const std::regex id("^[a-z][a-z0-9_]{0,63}$");
            if (schema.at("pattern") != "^[a-z][a-z0-9_]{0,63}$") throw std::logic_error("Unsupported bundled pattern");
            if (!std::regex_match(text, id)) fail("Expected stable ASCII ID");
        }
    }
    if (value.is_number()) {
        if (!value.is_number_integer()) fail("Integer token required");
        if (value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>(MaxRevision))
            fail("Integer overflow/limit");
        const auto number = value.get<std::int64_t>();
        if ((schema.contains("minimum") && number < schema.at("minimum").get<std::int64_t>()) ||
            (schema.contains("maximum") && number > schema.at("maximum").get<std::int64_t>())) fail("Integer outside schema bounds");
    }
}
Identity identity(const Json& value) { return {value.at("id").get<std::string>(), value.at("name").get<std::string>()}; }
Json identityJson(const Identity& value) { return {{"id", value.id}, {"name", value.name}}; }
}
Json parseDocument(std::string_view bytes) {
    if (bytes.size() > MaxConfigBytes) throw ConfigError("size_limit", "", "Configuration exceeds 256 KiB");
    if (bytes.substr(0, 3) == "\xef\xbb\xbf") throw ConfigError("invalid_text", "", "UTF-8 BOM forbidden");
    for (const unsigned char byte : bytes) if (byte < 0x20 && byte != '\t' && byte != '\r' && byte != '\n')
        throw ConfigError("invalid_text", "", "Raw NUL/control byte forbidden");
    std::map<int, std::set<std::string>> keys;
    try {
        return Json::parse(bytes.begin(), bytes.end(), [&](int depth, Json::parse_event_t event, Json& value) {
            using Event = Json::parse_event_t;
            if ((event == Event::object_start || event == Event::array_start) && depth >= 16)
                throw ConfigError("depth_limit", "", "Configuration nesting exceeds 16");
            if (event == Event::object_start) keys[depth] = {};
            if (event == Event::object_end) keys.erase(depth);
            if (event == Event::key) {
                const auto key = value.get<std::string>();
                checkText(key, "");
                if (!keys.at(depth - 1).insert(key).second)
                    throw ConfigError("duplicate_key", "/" + pointerToken(key), "Duplicate JSON member at depth " + std::to_string(depth));
            }
            if (event == Event::value && value.is_string()) checkText(value.get_ref<const std::string&>(), "");
            if (event == Event::value && value.is_number_float())
                throw ConfigError("integer_required", "", "Decimal/exponent/overflow numbers are unsupported in v1");
            return true;
        });
    } catch (const Json::exception& error) {
        throw ConfigError("malformed_json", "", error.what());
    }
}
void validateStructure(const Json& value) {
    if (value.is_object() && value.contains("schema_version") && value.at("schema_version").is_number_integer() && value.at("schema_version") != 1)
        throw ConfigError("unsupported_version", "/schema_version", "Only schema version 1 is supported");
    static const auto schema = Json::parse(SchemaJson);
    checkSchema(value, schema, schema, "");
}
Configuration decode(const Json& value) {
    Configuration result;
    result.schemaVersion = value.at("schema_version"); result.revision = value.at("revision");
    result.retiredIds = value.at("retired_ids").get<std::vector<std::string>>();
    const auto& system = value.at("system");
    static_cast<Identity&>(result.system) = identity(system);
    result.system.locale = system.at("locale"); result.system.interpretationProfile = system.at("interpretation_profile");
    for (const auto& item : value.at("rooms")) {
        Room room; static_cast<Identity&>(room) = identity(item);
        room.mentions = item.at("speech").at("mentions").get<std::vector<std::string>>();
        room.locations = item.at("speech").at("locations").get<std::vector<std::string>>();
        result.rooms.push_back(std::move(room));
    }
    for (const auto& item : value.at("devices")) {
        Device device; static_cast<Identity&>(device) = identity(item);
        device.kind = item.at("kind") == "light" ? DeviceKind::Light : DeviceKind::EntranceDoor;
        if (!item.at("room_id").is_null()) device.roomId = item.at("room_id").get<std::string>();
        device.enabled = item.at("enabled"); device.actionIds = item.at("action_ids").get<std::vector<std::string>>();
        result.devices.push_back(std::move(device));
    }
    for (const auto& item : value.at("actions")) {
        ActionDefinition action; static_cast<Identity&>(action) = identity(item);
        action.operation = item.at("operation") == "light.set_brightness.v1" ? Operation::SetBrightness : Operation::OpenEntranceDoor;
        action.enabled = item.at("enabled");
        for (const auto& entry : item.at("parameters")) {
            Parameter parameter; static_cast<Identity&>(parameter) = identity(entry);
            parameter.minimum = entry.at("minimum"); parameter.maximum = entry.at("maximum");
            action.parameters.push_back(std::move(parameter));
        }
        result.actions.push_back(std::move(action));
    }
    for (const auto& item : value.at("safety").at("restrictions")) {
        DenyAction deny; static_cast<Identity&>(deny) = identity(item);
        deny.deviceId = item.at("device_id"); deny.actionId = item.at("action_id");
        result.restrictions.push_back(std::move(deny));
    }
    const auto& mapping = value.at("compatibility");
    result.compatibility.lightActionId = mapping.at("light_action_id");
    result.compatibility.doorActionId = mapping.at("door_action_id");
    result.compatibility.doorTargetId = mapping.at("door_target_id");
    for (auto item = mapping.at("light_targets").begin(); item != mapping.at("light_targets").end(); ++item)
        result.compatibility.lightTargets[item.key()[0]] = item.value();
    return result;
}
std::string operationName(Operation value) {
    switch (value) {
        case Operation::Unknown: break;
        case Operation::SetBrightness: return "light.set_brightness.v1";
        case Operation::OpenEntranceDoor: return "entrance_door.open.v1";
    }
    throw ConfigError("unsupported_operation", "/actions", "Unknown operation enum");
}
std::string kindName(DeviceKind value) {
    switch (value) {
        case DeviceKind::Unknown: break;
        case DeviceKind::Light: return "light";
        case DeviceKind::EntranceDoor: return "entrance_door";
    }
    throw ConfigError("unsupported_kind", "/devices", "Unknown device kind enum");
}
Json encode(const Configuration& value) {
    Json result = {{"schema_version", value.schemaVersion}, {"revision", value.revision}, {"retired_ids", value.retiredIds}};
    auto system = identityJson(value.system);
    system["locale"] = value.system.locale; system["interpretation_profile"] = value.system.interpretationProfile;
    result["system"] = std::move(system);
    for (const auto* key : {"rooms", "devices", "actions", "bindings"}) result[key] = Json::array();
    for (const auto& room : value.rooms) {
        auto item = identityJson(room); item["speech"] = {{"mentions", room.mentions}, {"locations", room.locations}};
        result["rooms"].push_back(std::move(item));
    }
    for (const auto& device : value.devices) {
        auto item = identityJson(device);
        item["kind"] = kindName(device.kind); item["room_id"] = device.roomId ? Json(*device.roomId) : Json(nullptr);
        item["enabled"] = device.enabled; item["action_ids"] = device.actionIds; item["spoken_aliases"] = Json::array();
        result["devices"].push_back(std::move(item));
    }
    for (const auto& action : value.actions) {
        auto item = identityJson(action); item["operation"] = operationName(action.operation);
        item["enabled"] = action.enabled; item["parameters"] = Json::array(); item["speech_aliases"] = Json::object();
        for (const auto& parameter : action.parameters) {
            auto entry = identityJson(parameter);
            entry["type"] = "integer"; entry["required"] = true; entry["unit"] = "percent";
            entry["minimum"] = parameter.minimum; entry["maximum"] = parameter.maximum;
            item["parameters"].push_back(std::move(entry));
        }
        result["actions"].push_back(std::move(item));
    }
    result["safety"] = {{"restrictions", Json::array()}};
    for (const auto& deny : value.restrictions) {
        auto item = identityJson(deny); item["kind"] = "deny_action"; item["device_id"] = deny.deviceId; item["action_id"] = deny.actionId;
        result["safety"]["restrictions"].push_back(std::move(item));
    }
    const auto& mapping = value.compatibility;
    Json lights = Json::object();
    for (const auto& [code, target] : mapping.lightTargets) lights[std::string(1, code)] = target;
    result["compatibility"] = {{"codec", "compact_v1"}, {"light_action_id", mapping.lightActionId}, {"light_targets", lights},
        {"door_action_id", mapping.doorActionId}, {"door_target_id", mapping.doorTargetId}};
    return result;
}
}
