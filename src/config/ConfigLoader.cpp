#include "firefly/config/ConfigLoader.h"
#include "firefly/config/ConfigValidator.h"
#include "JsonInternal.h"
#include "Platform.h"
#include "ConfigSchema.h"
#include "firefly/config/Connectivity.h"
#include <algorithm>
#include <regex>
#include <cmath>
#include "firefly/actions/ScalarJson.h"

namespace firefly::config {
ConfigError::ConfigError(std::string codeValue, std::string pathValue, std::string message)
    : std::runtime_error(std::move(message)), code(std::move(codeValue)), path(std::move(pathValue)) {}
Catalog ConfigLoader::fromJson(std::string_view bytes) {
    const auto document = detail::parseDocument(bytes);
    detail::validateStructure(document);
    try { return ConfigValidator::validate(detail::decode(document)); }
    catch (const detail::Json::exception& error) { throw ConfigError("schema_error", "", error.what()); }
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
    if (type == "number") return value.is_number();
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
        if (!valid && types == "integer" && value.is_number_float()) throw ConfigError("integer_required", path, "Integer token required");
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
        const auto number = value.get<double>();
        if (!std::isfinite(number) || number < -static_cast<double>(MaxRevision) || number > static_cast<double>(MaxRevision))
            fail("Number outside exact browser-safe bounds");
        if ((schema.contains("minimum") && number < schema.at("minimum").get<double>()) ||
            (schema.contains("maximum") && number > schema.at("maximum").get<double>())) fail("Number outside schema bounds");
    }
}
Identity identity(const Json& value) { return {value.at("id").get<std::string>(), value.at("name").get<std::string>()}; }
Json identityJson(const Identity& value) { return {{"id", value.id}, {"name", value.name}}; }
EntityMetadata metadata(const Json& item) {
    EntityMetadata result;
    if (!item.contains("metadata")) return result;
    const auto& m = item.at("metadata"); result.present = true;
    result.origin = m.value("origin", ""); result.confirmed = m.value("confirmed", false);
    result.type = m.value("type", ""); result.description = m.value("description", "");
    result.aliases = m.value("aliases", std::vector<std::string>{});
    if (m.contains("parent_id") && !m.at("parent_id").is_null()) result.parentId = m.at("parent_id").get<std::string>();
    result.container = m.value("container", false);
    return result;
}
void encodeMetadata(Json& item, const EntityMetadata& m, bool location) {
    if (!location && (m.parentId || m.container)) throw ConfigError("schema_error", "/devices/metadata", "Device metadata cannot contain hierarchy fields");
    if (!m.present && m.type.empty() && m.description.empty() && m.aliases.empty() && !m.parentId && !m.container && m.origin.empty() && !m.confirmed) return;
    auto& value = item["metadata"];
    value = {{"type", m.type}, {"description", m.description}, {"aliases", m.aliases}};
    if (!m.origin.empty()) { value["origin"] = m.origin; value["confirmed"] = m.confirmed; }
    if (location) { value["parent_id"] = m.parentId ? Json(*m.parentId) : Json(nullptr); value["container"] = m.container; }
}

}
Json parseDocument(std::string_view bytes) {
    if (bytes.size() > MaxConfigBytes) throw ConfigError("size_limit", "", "Configuration exceeds 256 KiB");
    if (bytes.substr(0, 3) == "\xef\xbb\xbf") throw ConfigError("invalid_text", "", "UTF-8 BOM forbidden");
    for (const unsigned char byte : bytes) if (byte < 0x20 && byte != '\t' && byte != '\r' && byte != '\n')
        throw ConfigError("invalid_text", "", "Raw NUL/control byte forbidden");
    std::map<int, std::set<std::string>> keys;
    try {
        actions::checkNumericLexemes(bytes);
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

            return true;
        });
    } catch (const Json::exception& error) {
        throw ConfigError("malformed_json", "", error.what());
    } catch (const std::invalid_argument& error) { throw ConfigError("numeric_range", "", error.what()); }
}
void validateStructure(const Json& value) {
    const auto version = value.is_object() && value.contains("schema_version") ? value.at("schema_version") : Json(nullptr);
    if (!version.is_null() && version.is_number_integer() && version != 1 && version != 2)
        throw ConfigError("unsupported_version", "/schema_version", "Supported schema versions: 1, 2");
    static const auto v1 = Json::parse(SchemaJson), v2 = Json::parse(SchemaV2Json);
    const auto& schema = version == 2 ? v2 : v1;
    checkSchema(value, schema, schema, "");
}
Parameter decodeParameter(const Json& entry) {
    Parameter p; static_cast<Identity&>(p) = identity(entry);
    p.type = entry.at("type"); p.required = entry.at("required"); p.unit = entry.at("unit");
    p.description = entry.value("description", "");
    if (p.type == "integer") {
        if (!entry.contains("minimum") || !entry.contains("maximum") || !entry.at("minimum").is_number_integer() || !entry.at("maximum").is_number_integer())
            throw ConfigError("integer_required", "/parameters", "Integer bounds required");
        p.minimum = entry.at("minimum"); p.maximum = entry.at("maximum");
    } else if (p.type == "number") {
        p.numberMinimum = entry.at("minimum"); p.numberMaximum = entry.at("maximum");
    } else if (p.type == "string") {
        p.minLength = entry.at("min_length"); p.maxLength = entry.at("max_length");
    } else if (p.type == "enum") p.values = entry.at("values").get<std::vector<std::string>>();
    for (const auto* key : {"minimum", "maximum", "min_length", "max_length", "values"}) {
        const bool allowed = std::string(key) == "values" ? p.type == "enum" :
            std::string(key).find("length") != std::string::npos ? p.type == "string" : p.type == "integer" || p.type == "number";
        if (!allowed && entry.contains(key)) throw ConfigError("schema_error", "/parameters/" + std::string(key), "Constraint not applicable to scalar type");
    }
    return p;
}
HttpBinding decodeHttp(const Json& http, bool connected) {
    HttpBinding h;
    h.method = http.at("method"); h.url = http.value("url", ""); h.path = http.value("path", "");
    h.timeoutMs = http.value("timeout_ms", connected ? 0 : 2000);
    h.contentType = http.value("content_type", "application/json"); h.expectedStatus = http.value("expected_status", 0);
    if (http.contains("headers")) h.headers = http.at("headers").get<std::map<std::string, std::string>>();
    if (http.contains("argument")) h.argument = http.at("argument").get<std::string>();
    const auto body = http.value("body", Json::object());
    for (const auto& item : body.items()) {
        try { h.body.emplace(item.key(), actions::decodeScalar(item.value())); }
        catch (...) { throw ConfigError("malformed_binding", "/bindings/http/body", "Body values must be JSON scalars"); }
    }
    return h;
}
Json encodeHttp(const HttpBinding& h, bool connected) {
    Json body = Json::object();
    for (const auto& [key, scalar] : h.body) body[key] = actions::scalarJson(scalar);
    Json value{{"method",h.method},{"timeout_ms",h.timeoutMs},{"content_type",h.contentType},{"body",std::move(body)}};
    if (connected) { value["path"] = h.path; if (!h.url.empty()) value["url"] = h.url; }
    else { value["url"] = h.url; if (!h.path.empty()) value["path"] = h.path; }
    if (h.argument) value["argument"] = *h.argument;
    if (h.expectedStatus) value["expected_status"] = h.expectedStatus;
    if (!h.headers.empty()) value["headers"] = h.headers;
    return value;
}
Connection decodeConnection(const Json& item) {
    Connection c;
    static_cast<Identity&>(c) = identity(item);
    c.type = executorKind(item.at("type")); c.enabled = item.at("enabled"); c.timeoutMs = item.at("timeout_ms");
    const auto section = c.type == ExecutorKind::Http ? "http" : c.type == ExecutorKind::Serial ? "serial" : c.type == ExecutorKind::Can ? "can" : "network";
    if (!item.contains(section)) throw ConfigError("missing_field", "/connections", "Transport config required");
    for (const auto* key : {"http", "network", "serial", "can"})
        if (item.contains(key) && std::string(key) != section) throw ConfigError("schema_error", "/connections", "Wrong transport config");
    const auto& config = item.at(section);
    if (c.type == ExecutorKind::Http) { c.http.baseUrl = config.at("base_url"); c.http.basePath = config.value("base_path", ""); }
    else if (c.type == ExecutorKind::Serial) { c.serial.port = config.at("port"); c.serial.baudRate = config.at("baud_rate"); }
    else if (c.type == ExecutorKind::Can) c.can.interface = config.at("interface");
    else { c.network.host = config.at("host"); c.network.port = config.at("port"); }
    return c;
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
        room.metadata = metadata(item);
        room.mentions = item.at("speech").at("mentions").get<std::vector<std::string>>();
        room.locations = item.at("speech").at("locations").get<std::vector<std::string>>();
        result.rooms.push_back(std::move(room));
    }
    for (const auto& item : value.at("devices")) {
        Device device; static_cast<Identity&>(device) = identity(item);
        device.metadata = metadata(item);
        device.kind = item.at("kind") == "light" ? DeviceKind::Light : item.at("kind") == "generic" ? DeviceKind::Generic : DeviceKind::EntranceDoor;
        if (!item.at("room_id").is_null()) device.roomId = item.at("room_id").get<std::string>();
        device.enabled = item.at("enabled"); device.actionIds = item.at("action_ids").get<std::vector<std::string>>();
        device.spokenAliases = item.at("spoken_aliases").get<std::vector<std::string>>();
        if (result.schemaVersion == 2) for (const auto& entry : item.at("state")) {
            StateField field; static_cast<Parameter&>(field) = decodeParameter(entry);
            field.initial = actions::decodeTypedScalar(entry.at("initial"), field.type); device.state.push_back(std::move(field));
        }
        result.devices.push_back(std::move(device));
    }
    for (const auto& item : value.at("actions")) {
        ActionDefinition action; static_cast<Identity&>(action) = identity(item);
        action.operation = item.at("operation") == "light.set_brightness.v1" ? Operation::SetBrightness : item.at("operation") == "mock.assign.v1" ? Operation::AssignState : Operation::OpenEntranceDoor;
        action.enabled = item.at("enabled");
        for (const auto& entry : item.at("parameters")) {
            action.parameters.push_back(decodeParameter(entry));
        }
        if (result.schemaVersion == 2) {
            action.description = item.at("description"); action.origin = item.value("origin", "");
            action.aliases = item.at("speech_aliases").get<std::vector<std::string>>();
            for (const auto& entry : item.at("assignments")) {
                Assignment assignment; assignment.field = entry.at("field");
                if (entry.contains("argument")) assignment.argument = entry.at("argument").get<std::string>();
                if (entry.contains("value")) assignment.value = actions::decodeScalar(entry.at("value"));
                action.assignments.push_back(std::move(assignment));
            }
        }
        result.actions.push_back(std::move(action));
    }
    for (const auto& item : value.at("safety").at("restrictions")) {
        DenyAction deny; static_cast<Identity&>(deny) = identity(item);
        deny.deviceId = item.at("device_id"); deny.actionId = item.at("action_id");
        result.restrictions.push_back(std::move(deny));
    }
    if (result.schemaVersion == 2) {
        for (const auto& item : value.value("connections", Json::array())) {
            auto c = decodeConnection(item);
            result.connections.push_back(std::move(c));
        }
        for (const auto& item : value.value("observations", Json::array())) {
            ObservationBinding o;
            o.targetId = item.at("target_id"); o.connectionId = item.at("connection_id");
            o.executor = executorKind(item.at("executor"));
            o.http = decodeHttp(item.at("http"), true);
            o.fields = item.at("fields").get<std::map<std::string, std::string>>();
            result.observations.push_back(std::move(o));
        }
        for (const auto& item : value.at("bindings")) {
            Binding binding;
            binding.targetId = item.at("target_id");
            binding.actionId = item.at("action_id");
            binding.executor = executorKind(item.at("executor"));
            binding.connectionId = item.value("connection_id", "");
            binding.http = decodeHttp(item.at("http"), !binding.connectionId.empty());
            result.bindings.push_back(std::move(binding));
        }
    }
    const auto& mapping = value.at("compatibility");
    if (!mapping.is_null()) {
        result.compatibility.lightActionId = mapping.at("light_action_id");
        result.compatibility.doorActionId = mapping.at("door_action_id");
        result.compatibility.doorTargetId = mapping.at("door_target_id");
        for (auto item = mapping.at("light_targets").begin(); item != mapping.at("light_targets").end(); ++item)
            result.compatibility.lightTargets[item.key()[0]] = item.value();
    }
    return result;
}
std::string operationName(Operation value) {
    switch (value) {
        case Operation::Unknown: break;
        case Operation::AssignState: return "mock.assign.v1";
        case Operation::SetBrightness: return "light.set_brightness.v1";
        case Operation::OpenEntranceDoor: return "entrance_door.open.v1";
    }
    throw ConfigError("unsupported_operation", "/actions", "Unknown operation enum");
}
std::string kindName(DeviceKind value) {
    switch (value) {
        case DeviceKind::Unknown: break;
        case DeviceKind::Light: return "light";
        case DeviceKind::Generic: return "generic";
        case DeviceKind::EntranceDoor: return "entrance_door";
    }
    throw ConfigError("unsupported_kind", "/devices", "Unknown device kind enum");
}
Json encodeParameter(const Parameter& p, bool v2) {
    auto entry = identityJson(p);
    entry["type"] = p.type; entry["required"] = p.required; entry["unit"] = p.unit;
    if (p.type == "integer") { entry["minimum"] = p.minimum; entry["maximum"] = p.maximum; }
    if (p.type == "number") { entry["minimum"] = p.numberMinimum; entry["maximum"] = p.numberMaximum; }
    if (p.type == "string") { entry["min_length"] = p.minLength; entry["max_length"] = p.maxLength; }
    if (p.type == "enum") entry["values"] = p.values;
    if (v2) entry["description"] = p.description;
    return entry;
}
Json encode(const Configuration& value) {
    Json result = {{"schema_version", value.schemaVersion}, {"revision", value.revision}, {"retired_ids", value.retiredIds}};
    auto system = identityJson(value.system);
    system["locale"] = value.system.locale; system["interpretation_profile"] = value.system.interpretationProfile;
    result["system"] = std::move(system);
    for (const auto* key : {"rooms", "devices", "actions", "bindings"}) result[key] = Json::array();
    for (const auto& room : value.rooms) {
        auto item = identityJson(room); item["speech"] = {{"mentions", room.mentions}, {"locations", room.locations}};
        encodeMetadata(item, room.metadata, true);
        result["rooms"].push_back(std::move(item));
    }
    for (const auto& device : value.devices) {
        auto item = identityJson(device);
        item["kind"] = kindName(device.kind); item["room_id"] = device.roomId ? Json(*device.roomId) : Json(nullptr);
        item["enabled"] = device.enabled; item["action_ids"] = device.actionIds; item["spoken_aliases"] = device.spokenAliases;
        if (value.schemaVersion == 2) {
            item["state"] = Json::array();
            for (const auto& field : device.state) {
                auto entry = encodeParameter(field, true); entry["initial"] = actions::scalarJson(field.initial);
                item["state"].push_back(std::move(entry));
            }
        }
        encodeMetadata(item, device.metadata, false);
        result["devices"].push_back(std::move(item));
    }
    for (const auto& action : value.actions) {
        auto item = identityJson(action); item["operation"] = operationName(action.operation);
        item["enabled"] = action.enabled; item["parameters"] = Json::array(); item["speech_aliases"] = Json::object();
        for (const auto& parameter : action.parameters) {
            item["parameters"].push_back(encodeParameter(parameter, value.schemaVersion == 2));
        }
        if (value.schemaVersion == 2) {
            if (!action.origin.empty()) item["origin"] = action.origin;
            item["description"] = action.description; item["speech_aliases"] = action.aliases;
            item["assignments"] = Json::array();
            for (const auto& assignment : action.assignments) {
                Json entry{{"field", assignment.field}};
                if (assignment.argument) entry["argument"] = *assignment.argument;
                if (assignment.value) entry["value"] = actions::scalarJson(*assignment.value);
                item["assignments"].push_back(std::move(entry));
            }
        }
        result["actions"].push_back(std::move(item));
    }
    result["safety"] = {{"restrictions", Json::array()}};
    for (const auto& deny : value.restrictions) {
        auto item = identityJson(deny); item["kind"] = "deny_action"; item["device_id"] = deny.deviceId; item["action_id"] = deny.actionId;
        result["safety"]["restrictions"].push_back(std::move(item));
    }
    if (value.schemaVersion == 2) {
        // Omit empty extension arrays to preserve existing on-disk canonical hashes.
        if (!value.connections.empty()) result["connections"] = Json::array();
        for (const auto& c : value.connections) {
            Json entry{{"id",c.id},{"name",c.name},{"type",executorName(c.type)}, {"enabled",c.enabled},{"timeout_ms",c.timeoutMs}};
            if (c.type == ExecutorKind::Http) entry["http"] = {{"base_url",c.http.baseUrl},{"base_path",c.http.basePath}};
            else if (c.type == ExecutorKind::Serial) entry["serial"] = {{"port",c.serial.port},{"baud_rate",c.serial.baudRate}};
            else if (c.type == ExecutorKind::Can) entry["can"] = {{"interface",c.can.interface}};
            else entry["network"] = {{"host",c.network.host},{"port",c.network.port}};
            result["connections"].push_back(std::move(entry));
        }
        if (!value.observations.empty()) result["observations"] = Json::array();
        for (const auto& o : value.observations)
            result["observations"].push_back({{"target_id",o.targetId},{"connection_id",o.connectionId},{"executor",executorName(o.executor)},
                {"http",encodeHttp(o.http,true)},{"fields",o.fields}});
        for (const auto& binding : value.bindings) {
            Json http = encodeHttp(binding.http, !binding.connectionId.empty());
            Json entry{{"target_id", binding.targetId}, {"action_id", binding.actionId},
                {"executor", executorName(binding.executor)}, {"http", std::move(http)}};
            if (!binding.connectionId.empty()) entry["connection_id"] = binding.connectionId;
            result["bindings"].push_back(std::move(entry));
        }
    }
    const auto& mapping = value.compatibility;
    Json lights = Json::object();
    for (const auto& [code, target] : mapping.lightTargets) lights[std::string(1, code)] = target;
    result["compatibility"] = {{"codec", "compact_v1"}, {"light_action_id", mapping.lightActionId}, {"light_targets", lights},
        {"door_action_id", mapping.doorActionId}, {"door_target_id", mapping.doorTargetId}};
    if (value.schemaVersion == 2 && mapping.lightActionId.empty() && mapping.doorActionId.empty() && mapping.doorTargetId.empty() && mapping.lightTargets.empty()) result["compatibility"] = nullptr;
    return result;
}
}

namespace firefly::config {
Connection ConfigLoader::connectionFromJson(std::string_view bytes) {
    const auto value = detail::parseDocument(bytes);
    static const auto schema = detail::Json::parse(detail::SchemaV2Json);
    detail::checkSchema(value, schema.at("$defs").at("connection"), schema, "/connection");
    auto connection = detail::decodeConnection(value);
    ConfigValidator::validateIdentityName(connection, "/connection");
    Configuration scoped;
    scoped.schemaVersion = 2;
    scoped.connections.push_back(connection);
    try { validateConnections(scoped); }
    catch (const ConfigError& error) {
        const auto suffix = error.path.rfind("/connections/0", 0) == 0 ? error.path.substr(14) : std::string{};
        throw ConfigError(error.code, "/connection" + suffix, error.what());
    }
    return connection;
}
}
