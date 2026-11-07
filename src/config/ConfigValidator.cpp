#include "firefly/config/ConfigValidator.h"
#include "firefly/config/Connectivity.h"
#include "JsonInternal.h"
#include "Platform.h"
#include "firefly/actions/ScalarJson.h"
#include <algorithm>
#include <arpa/inet.h>
#include <sstream>
#include <cmath>
#include <string_view>

namespace firefly::config {
namespace {
template<class T> void sortIdentities(std::vector<T>& values) {
    std::sort(values.begin(), values.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
}
std::string normalizedAlias(const std::string& value) {
    std::istringstream input(detail::lowercase(value));
    std::string word, result;
    while (input >> word) { if (!result.empty()) result += ' '; result += word; }
    return result;
}
bool containsPhrase(const std::string& phrase, const std::string& part) {
    return (" " + phrase + " ").find(" " + part + " ") != std::string::npos;
}
void checkSpokenAliases(const std::vector<std::string>& aliases, const std::string& path, bool device) {
    static const std::set<std::string> intentOnly{
        "не", "нет", "ни", "и", "или", "либо", "да", "true", "false",
        "включи", "включить", "включите", "выключи", "выключить", "выключите",
        "открой", "открыть", "откройте", "закрой", "запусти", "останови",
        "установи", "поставь", "выбери", "переключи", "пожалуйста"};
    for (const auto& alias : aliases) {
        const auto normalized = normalizedAlias(alias);
        if (normalized.empty() || std::any_of(alias.begin(),alias.end(),[](unsigned char c){return c < 32 || c == 127;}))
            throw ConfigError("invalid_alias",path,"Spoken alias must contain words, without control characters");
        if (device) {
            std::istringstream words(normalized);std::string word;bool content = false;
            while (words >> word) content = content || !intentOnly.count(word);
            if (!content) throw ConfigError("reserved_alias",path,"Device grounding cannot consist only of intent/control words");
        }
    }
}
void checkAlias(const std::string& alias, const std::string& path, bool mention, bool v2 = false) {
    if (alias.empty() || alias.size() > 96 || normalizedAlias(alias) != alias)
        throw ConfigError("invalid_alias", path, "Alias must be canonical lowercase, single spaces, at most 96 bytes");
    for (const unsigned char c : alias) {
        if (c < 0x80 && c != ' ' && !(c >= 'a' && c <= 'z') && !(v2 && c >= '0' && c <= '9'))
            throw ConfigError("invalid_alias", path, "Alias must contain only words and spaces");
    }
    if (mention && alias.find(' ') != std::string::npos)
        throw ConfigError("invalid_alias", path, "Legacy room mention must be one token");
    static const std::set<std::string> reserved{
        "не", "нет", "ни", "или", "и", "u", "d", "quit", "exit", "игнорируй",
        "открой", "откройте", "открыть", "открывай", "открывайте", "закрой", "разблокируй",
        "включи", "включить", "выключи", "выключить", "свет", "яркость", "наполовину", "максимум",
        "дверь", "двер", "входная", "входную", "входе"};
    std::istringstream words(alias);
    std::string word;
    while (words >> word) if (reserved.count(word))
        throw ConfigError("reserved_alias", path, "Reserved grammar/security token: " + word);
}
void addIdentity(const Identity& value, const std::string& path, std::map<std::string, std::string>& ids) {
    ConfigValidator::validateIdentityName(value, path);
    const auto inserted = ids.emplace(value.id, path);
    if (!inserted.second) throw ConfigError("duplicate_id", path + "/id", "ID also appears at " + inserted.first->second + "/id");
}
struct LegacyRoom {
    char code;
    std::string room, device;
    std::vector<std::string> mentions, locations;
};
// Versioned compatibility contract, intentionally checked against the frozen
// source by separate shadow tests. Production consumers do not use this table.
const std::vector<LegacyRoom>& legacyRooms() {
    static const std::vector<LegacyRoom> rooms{
        {'K', "room_kitchen", "device_kitchen_light", {"кухня", "кухне", "кухню", "кухни", "кухней"}, {"на кухне"}},
        {'B', "room_bedroom", "device_bedroom_light", {"спальня", "спальне", "спальню", "спальни", "спальней"}, {"в спальне"}},
        {'G', "room_living", "device_living_light", {"гостиная", "гостиной", "гостиную", "зал", "зале", "зала", "залу", "залом"}, {"в гостиной", "в зале"}},
        {'C', "room_corridor", "device_corridor_light", {"коридор", "коридоре", "коридора", "коридору", "коридором"}, {"в коридоре"}}
    };
    return rooms;
}
bool sameStrings(std::vector<std::string> a, std::vector<std::string> b) {
    std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end()); return a == b;
}

}
namespace {
void parameterContract(const Parameter& p, const std::string& path) {
    if ((p.type == "integer" && p.minimum > p.maximum) ||
        (p.type == "number" && (!std::isfinite(p.numberMinimum) || !std::isfinite(p.numberMaximum) || p.numberMinimum > p.numberMaximum)) ||
        (p.type == "string" && p.minLength > p.maxLength))
        throw ConfigError("unsafe_constraint", path, "Minimum exceeds maximum or is non-finite");
    if (p.type == "enum") {
        const std::set<std::string> values(p.values.begin(), p.values.end());
        if (values.empty() || values.size() != p.values.size())
            throw ConfigError("unsafe_constraint", path, "Enum requires distinct nonempty choices");
    }
}
}
void ConfigValidator::validateIdentityName(const Identity& value, const std::string& path) {
    if (value.name.size() > 128 || value.name.find_first_not_of(' ') == std::string::npos)
        throw ConfigError("invalid_name", path + "/name", "Display name must be nonblank and at most 128 UTF-8 bytes");
}
Catalog ConfigValidator::validate(Configuration configuration) {
    if (configuration.schemaVersion == 1) {
        for (const auto& d : configuration.devices) if (!d.state.empty() || !d.spokenAliases.empty())
            throw ConfigError("unsupported_version", "/devices", "Typed state and speech bindings require migration to v2");
        for (const auto& a : configuration.actions) if (!a.assignments.empty() || !a.aliases.empty() || !a.description.empty() || !a.origin.empty())
            throw ConfigError("unsupported_version", "/actions", "Action extensions require migration to v2");
    }
    const auto encoded = detail::encode(configuration);
    try { actions::checkNumericLexemes(encoded.dump()); } catch (const std::exception& error) { throw ConfigError("unsafe_constraint","",error.what()); }
    detail::validateStructure(encoded);
    auto catalog = std::shared_ptr<ValidatedCatalog>(new ValidatedCatalog);
    std::map<std::string, std::string> ids;
    addIdentity(configuration.system, "/system", ids);
    const auto& voice = configuration.system.voiceDialogue;
    if (voice.responses != "off" && voice.responses != "brief" && voice.responses != "detailed")
        throw ConfigError("schema_error", "/system/voice_dialogue/responses", "Unsupported voice response mode");
    if (!voice.present && (voice.responses != "off" || voice.questionsEnabled))
        throw ConfigError("schema_error", "/system/voice_dialogue", "Explicit preferences required");
    if (!configuration.system.voiceInput.present && configuration.system.voiceInput.inlineCommands)
        throw ConfigError("schema_error", "/system/voice_input", "Explicit voice input preference required");
    const auto identify = [&](const auto& values, const std::string& path) {
        for (std::size_t i = 0; i < values.size(); ++i) addIdentity(values[i], path + "/" + std::to_string(i), ids);
    };
    identify(configuration.connections, "/connections");
    validateConnections(configuration);
    identify(configuration.rooms, "/rooms"); identify(configuration.devices, "/devices");
    identify(configuration.actions, "/actions"); identify(configuration.restrictions, "/safety/restrictions");
    std::set<std::string> retired;
    for (const auto& id : configuration.retiredIds) {
        if (!retired.insert(id).second) throw ConfigError("duplicate_id", "/retired_ids", "Duplicate retired ID: " + id);
        if (ids.count(id)) throw ConfigError("retired_id", "/retired_ids", "Active object uses retired ID: " + id);
    }
    std::map<std::string, std::string> mentionPaths, locationPaths;
    for (std::size_t i = 0; i < configuration.rooms.size(); ++i) {
        const auto& room = configuration.rooms[i];
        if (!room.metadata.container && (room.mentions.empty() || room.locations.empty()))
            throw ConfigError("invalid_alias", "/rooms/" + std::to_string(i), "Command location requires explicit mentions and phrases; use a container for grouping only.");
        if (room.metadata.container && (!room.mentions.empty() || !room.locations.empty()))
            throw ConfigError("invalid_alias", "/rooms/" + std::to_string(i), "Container locations have no command aliases.");
        const auto addAliases = [&](const auto& aliases, auto& index, auto& paths, const std::string& role) {
            for (std::size_t j = 0; j < aliases.size(); ++j) {
                const auto path = "/rooms/" + std::to_string(i) + "/speech/" + role + "/" + std::to_string(j);
                const auto key = normalizedAlias(aliases[j]);
                const auto inserted = paths.emplace(key, path);
                if (!inserted.second) throw ConfigError("ambiguous_alias", path, "Alias also appears at " + inserted.first->second);
                checkAlias(aliases[j], path, role == "mentions" && configuration.schemaVersion == 1, configuration.schemaVersion == 2);
                index.emplace(key, room.id);
            }
        };
        addAliases(room.mentions, catalog->mentions_, mentionPaths, "mentions");
        addAliases(room.locations, catalog->locations_, locationPaths, "locations");
    }
    if (configuration.schemaVersion == 2) {
        auto aliases = catalog->mentions_;
        for (const auto& location : configuration.rooms) if (!location.metadata.container)
            for (const auto& alias : location.metadata.aliases) {
                const auto normalized = normalizedAlias(alias);
                const auto path = "/rooms/" + location.id + "/metadata/aliases";
                checkAlias(normalized,path,false,true);
                const auto inserted = aliases.emplace(normalized,location.id);
                if (!inserted.second && inserted.first->second != location.id)
                    throw ConfigError("ambiguous_alias",path,"Location alias already belongs to " + inserted.first->second);
            }
    }
    // Mentions and location phrases are independent speech aliases owned by the
    // same Location. Do not require a literal substring link between forms
    // ("кабинет 1" / "в кабинете 1" is valid). Safety is ownership plus
    // fail-closed collision checks: a phrase must not embed another room's
    // mention or location alias, and same-role duplicates are already rejected.
    for (const auto& [phrase, room] : catalog->locations_) {
        for (const auto& [mention, target] : catalog->mentions_) if (containsPhrase(phrase, mention)) {
            if (room != target) throw ConfigError("ambiguous_alias", locationPaths.at(phrase), "Location contains another room's mention at " + mentionPaths.at(mention));
        }
        for (const auto& [other, target] : catalog->locations_) if (room != target && containsPhrase(phrase, other))
            throw ConfigError("ambiguous_alias", locationPaths.at(phrase), "Overlapping location at " + locationPaths.at(other));
    }
    sortIdentities(configuration.connections);
    std::sort(configuration.observations.begin(), configuration.observations.end(), [](const auto& a, const auto& b) { return a.targetId < b.targetId; });
    sortIdentities(configuration.rooms); sortIdentities(configuration.devices);
    sortIdentities(configuration.actions); sortIdentities(configuration.restrictions);
    std::sort(configuration.retiredIds.begin(), configuration.retiredIds.end());
    std::sort(configuration.bindings.begin(), configuration.bindings.end(), [](const Binding& a, const Binding& b) {
        return std::tie(a.targetId, a.actionId) < std::tie(b.targetId, b.actionId);
    });
    for (std::size_t i = 0; i < configuration.rooms.size(); ++i) {
        auto& room = configuration.rooms[i];
        std::sort(room.mentions.begin(), room.mentions.end()); std::sort(room.locations.begin(), room.locations.end());
        catalog->rooms_.emplace(room.id, i);
    }
    for (const auto& location : configuration.rooms) {
        std::set<std::string> seen{location.id};
        auto parent = location.metadata.parentId;
        while (parent) {
            if (!catalog->rooms_.count(*parent)) throw ConfigError("dangling_reference", "/rooms/" + location.id + "/metadata/parent_id", "Unknown parent location: " + *parent);
            if (!seen.insert(*parent).second) throw ConfigError("hierarchy_cycle", "/rooms/" + location.id, "Location hierarchy contains a cycle");
            parent = configuration.rooms[catalog->rooms_.at(*parent)].metadata.parentId;
        }
    }
    for (std::size_t i = 0; i < configuration.actions.size(); ++i) {
        auto& action = configuration.actions[i];
        const auto& path = ids.at(action.id);
        catalog->actions_.emplace(action.id, i);
        std::map<std::string, std::string> parameterIds;
        for (std::size_t j = 0; j < action.parameters.size(); ++j)
            addIdentity(action.parameters[j], path + "/parameters/" + std::to_string(j), parameterIds);
        for (const auto& p : action.parameters) parameterContract(p, path + "/parameters/" + p.id);
        if (action.operation == Operation::AssignState) {
            if (action.assignments.empty() || action.aliases.empty()) throw ConfigError("unsupported_operation", path, "Mock action needs explicit language and assignments");
            checkSpokenAliases(action.aliases,path + "/speech_aliases",false);
            std::set<std::string> fields;
            for (const auto& assignment : action.assignments) {
                if (assignment.argument.has_value() == assignment.value.has_value() || !fields.insert(assignment.field).second)
                    throw ConfigError("invalid_assignment", path, "Each field needs exactly one source and one assignment");
                if (assignment.argument && !parameterIds.count(*assignment.argument))
                    throw ConfigError("dangling_reference", path, "Assignment refers to unknown argument");
            }
        } else if (!action.assignments.empty()) throw ConfigError("unsafe_constraint", path, "Protected operation behavior is compiled and cannot be replaced");
        if (action.operation == Operation::SetBrightness) {
            if (action.parameters.size() != 1 || action.parameters[0].id != "brightness")
                throw ConfigError("unsupported_operation", path, "Brightness requires exactly its brightness parameter");
            const auto& p = action.parameters[0];
            if (p.type != "integer" || !p.required || p.unit != "percent" || p.minimum < 0 || p.maximum > 100)
                throw ConfigError("unsafe_constraint", path, "Brightness requires a required integer percentage in 0..100");
            if (action.parameters[0].minimum > action.parameters[0].maximum)
                throw ConfigError("unsafe_constraint", path, "Minimum exceeds maximum");
        } else if (action.operation == Operation::OpenEntranceDoor && !action.parameters.empty()) throw ConfigError("unsafe_constraint", path, "Entrance opening accepts no parameters");
    }
    for (std::size_t i = 0; i < configuration.devices.size(); ++i) {
        auto& device = configuration.devices[i];
        const auto& path = ids.at(device.id);
        if (configuration.schemaVersion == 2) {
            checkSpokenAliases(device.spokenAliases,path + "/spoken_aliases",true);
            checkSpokenAliases(device.metadata.aliases,path + "/metadata/aliases",true);
        }
        catalog->devices_.emplace(device.id, i);
        if (device.roomId && !catalog->rooms_.count(*device.roomId)) throw ConfigError("dangling_reference", path + "/room_id", "Unknown room");
        if (device.kind == DeviceKind::Light && !device.roomId) throw ConfigError("unsafe_constraint", path, "A light requires an explicit room");
        if (device.kind == DeviceKind::EntranceDoor && device.roomId) throw ConfigError("unsafe_constraint", path, "The legacy entrance door has no room association");
        if (configuration.schemaVersion == 1 && device.kind == DeviceKind::Generic && !device.actionIds.empty())
            throw ConfigError("unsupported_operation", path, "Generic inventory devices have no registered executable capabilities");
        if (device.kind != DeviceKind::Generic && device.actionIds.empty())
            throw ConfigError("unsafe_constraint", path, "Executable device requires a capability");
        if (device.kind == DeviceKind::Light && configuration.rooms[catalog->rooms_.at(*device.roomId)].metadata.container)
            throw ConfigError("unsafe_constraint", path, "Select a command location for this light, not a grouping container");
        std::map<std::string, const StateField*> fields;
        if (device.kind != DeviceKind::Generic && !device.state.empty())
            throw ConfigError("unsafe_constraint", path, "Protected device state cannot be replaced");
        for (const auto& field : device.state) {
            if (!fields.emplace(field.id, &field).second) throw ConfigError("duplicate_id", path + "/state", "Duplicate state field");
            parameterContract(field, path + "/state/" + field.id);
            if (!field.required || !accepts(field, field.initial)) throw ConfigError("invalid_initial_state", path + "/state/" + field.id, "Initial state does not match its declared schema");
        }
        std::set<std::string> actionIds;
        std::set<Operation> operations;
        for (const auto& id : device.actionIds) {
            if (!actionIds.insert(id).second) throw ConfigError("duplicate_reference", path + "/action_ids", "Duplicate action reference");
            if (!catalog->actions_.count(id)) throw ConfigError("dangling_reference", path + "/action_ids", "Unknown action: " + id);
            const auto& action = configuration.actions[catalog->actions_.at(id)];
            if (action.operation != Operation::AssignState && !operations.insert(action.operation).second) throw ConfigError("ambiguous_operation", path, "Multiple definitions of the same operation on one target");
            if ((device.kind == DeviceKind::Light && action.operation != Operation::SetBrightness) ||
                (device.kind == DeviceKind::EntranceDoor && action.operation != Operation::OpenEntranceDoor) ||
                (device.kind == DeviceKind::Generic && action.operation != Operation::AssignState))
                throw ConfigError("unsafe_constraint", path + "/action_ids", "Operation does not match device kind");
            for (const auto& assignment : action.assignments) {
                const auto field = fields.find(assignment.field);
                if (field == fields.end()) throw ConfigError("dangling_reference", path + "/state", "Action assigns an undeclared field: " + assignment.field);
                if (assignment.value && !accepts(*field->second, *assignment.value))
                    throw ConfigError("invalid_assignment", path, "Fixed assignment violates state schema");
                if (assignment.argument) {
                    const auto p = std::find_if(action.parameters.begin(), action.parameters.end(), [&](const auto& p) { return p.id == *assignment.argument; });
                    if (p == action.parameters.end() || p->type != field->second->type)
                        throw ConfigError("invalid_assignment", path, "Argument and state types differ");
                }
            }
        }
        std::sort(device.actionIds.begin(), device.actionIds.end());
    }
    std::set<std::pair<std::string, std::string>> denied;
    for (const auto& deny : configuration.restrictions) {
        if (!catalog->devices_.count(deny.deviceId) || !catalog->actions_.count(deny.actionId))
            throw ConfigError("dangling_reference", "/safety/restrictions", "Unknown deny target/action");
        const auto& device = configuration.devices[catalog->devices_.at(deny.deviceId)];
        if (std::find(device.actionIds.begin(), device.actionIds.end(), deny.actionId) == device.actionIds.end())
            throw ConfigError("dangling_reference", "/safety/restrictions", "Denied action is not on target");
        if (!denied.emplace(deny.deviceId, deny.actionId).second)
            throw ConfigError("duplicate_restriction", "/safety/restrictions", "Duplicate effective deny");
    }
    if (configuration.schemaVersion == 1 && !configuration.bindings.empty())
        throw ConfigError("unsupported_version", "/bindings", "Hardware bindings require schema v2");
    for (std::size_t i = 0; i < configuration.bindings.size(); ++i) {
        const auto& binding = configuration.bindings[i];
        const auto path = "/bindings/" + std::to_string(i);
        if (binding.executor != ExecutorKind::Http && binding.executor != ExecutorKind::Mqtt && !isModbus(binding.executor))
            throw ConfigError("unsupported_executor", path + "/executor", "Unsupported hardware executor");
        if (!catalog->devices_.count(binding.targetId))
            throw ConfigError("dangling_reference", path + "/target_id", "Unknown binding target");
        if (!catalog->actions_.count(binding.actionId))
            throw ConfigError("dangling_reference", path + "/action_id", "Unknown binding action");
        const auto& device = configuration.devices[catalog->devices_.at(binding.targetId)];
        if (std::find(device.actionIds.begin(), device.actionIds.end(), binding.actionId) == device.actionIds.end())
            throw ConfigError("dangling_reference", path, "Binding action is not available on the target");
        if (denied.count({binding.targetId, binding.actionId}))
            throw ConfigError("unsafe_constraint", path, "Binding cannot map a denied target/action pair");
        if (!device.enabled || !configuration.actions[catalog->actions_.at(binding.actionId)].enabled)
            throw ConfigError("unsafe_constraint", path, "Binding requires an enabled target and action");
        const auto key = std::make_pair(binding.targetId, binding.actionId);
        if (!catalog->bindings_.emplace(key, i).second)
            throw ConfigError("duplicate_binding", path, "Duplicate binding for the same target and action");
        const auto& http = binding.http;
        const auto* connection = findConnection(configuration, binding.connectionId);
        if (!binding.connectionId.empty() && !connection)
            throw ConfigError("dangling_reference", path + "/connection_id", "Unknown connection");
        if (binding.executor == ExecutorKind::Mqtt) {
            if (binding.connectionId.empty()) throw ConfigError("malformed_binding", path, "MQTT connection_id required");
            // MVP supports only fixed payloads. A parameterized action must not
            // silently send an unrelated constant while predicting its argument.
            if (!configuration.actions[catalog->actions_.at(binding.actionId)].parameters.empty())
                throw ConfigError("unsupported_executor", path + "/mqtt",
                                  "Parameterized MQTT actions require a reviewed typed argument mapping");
            validateMqttBinding(binding.mqtt, connection);
        } else if(isModbus(binding.executor)) {
            validateModbusBinding(binding.modbus,connection,binding.executor,configuration.actions[catalog->actions_.at(binding.actionId)]);
        } else validateHttpMapping(http, connection);
        if (binding.executor == ExecutorKind::Http && http.argument) {
            const auto& action = configuration.actions[catalog->actions_.at(binding.actionId)];
            const auto found = std::find_if(action.parameters.begin(), action.parameters.end(),
                [&](const auto& p) { return p.id == *http.argument; });
            if (found == action.parameters.end())
                throw ConfigError("dangling_reference", path + "/http/argument", "HTTP argument is not an action parameter");
            if (http.body.count(*http.argument))
                throw ConfigError("malformed_binding", path + "/http/argument", "HTTP argument collides with a fixed body field");
        }
    }
    std::set<std::string> observedTargets;
    for (const auto& observation : configuration.observations) {
        if (!catalog->devices_.count(observation.targetId)) throw ConfigError("dangling_reference", "/observations", "Unknown observe target");
        const auto& device = configuration.devices[catalog->devices_.at(observation.targetId)];
        const auto* connection = findConnection(configuration, observation.connectionId);
        if (!connection) throw ConfigError("dangling_reference", "/observations/connection_id", "Unknown observe connection");
        if (!device.enabled || (observation.executor != ExecutorKind::Http && observation.executor != ExecutorKind::Mqtt && !isModbus(observation.executor)))
            throw ConfigError("unsafe_constraint", "/observations", "Observe target must be enabled and transport supported");
        if (!observedTargets.insert(device.id).second) throw ConfigError("duplicate_binding", "/observations", "Only one observation mapping per device");
        if (observation.executor == ExecutorKind::Mqtt) {
            validateMqttObservation(observation.mqtt, connection);
            if (observation.mqtt.format == "text") {
                if (!observation.fields.empty()) throw ConfigError("malformed_binding", "/observations/fields", "Text MQTT observe uses mqtt.field, not fields");
                const auto field = std::find_if(device.state.begin(),device.state.end(), [&](const StateField& f) { return f.id == observation.mqtt.field; });
                if (device.kind != DeviceKind::Generic || field == device.state.end() || field->type != "boolean")
                    throw ConfigError("unsafe_constraint", "/observations/mqtt/field", "Text MQTT observation requires a declared boolean property");
            }
        } else if(isModbus(observation.executor)) {
            if(!observation.fields.empty())throw ConfigError("malformed_binding","/observations/fields","Modbus uses modbus.field, not fields");
            validateModbusObservation(observation.modbus,connection,observation.executor,device);
        } else validateHttpMapping(observation.http, connection, true);
        if (observation.executor == ExecutorKind::Http || observation.mqtt.format == "json") {
            if (observation.fields.empty() || observation.fields.size() > 32)
                throw ConfigError("malformed_binding", "/observations/fields", "Observe fields required (max 32)");
        }
        std::set<std::string> fields;
        for (const auto& [source, destination] : observation.fields) {
            if (source.empty() || source.size() > 64 || !fields.insert(destination).second)
                throw ConfigError("malformed_binding", "/observations/fields", "Distinct state fields required");
            bool exists = device.kind == DeviceKind::Light ? destination == "brightness" : device.kind == DeviceKind::EntranceDoor ? destination == "position" : false;
            for (const auto& field : device.state) if (field.id == destination) exists = true;
            if (!exists) throw ConfigError("dangling_reference", "/observations/fields", "Unknown state field");
        }
    }
    const auto& mapping = configuration.compatibility;
    const bool noLegacy = configuration.schemaVersion == 2 && mapping.lightActionId.empty() && mapping.doorActionId.empty() && mapping.doorTargetId.empty() && mapping.lightTargets.empty();
    if (!noLegacy) {
        if (!catalog->actions_.count(mapping.lightActionId) || !catalog->actions_.count(mapping.doorActionId) || !catalog->devices_.count(mapping.doorTargetId))
            throw ConfigError("dangling_reference", "/compatibility", "Unknown legacy action/door target");
        if (mapping.lightActionId != "action_set_brightness" || mapping.doorActionId != "action_open_entrance" || mapping.doorTargetId != "device_entrance_door")
            throw ConfigError("legacy_incompatible", "/compatibility", "Legacy stable action/door identities cannot be reassigned");
        if (configuration.actions[catalog->actions_.at(mapping.lightActionId)].operation != Operation::SetBrightness ||
            configuration.actions[catalog->actions_.at(mapping.doorActionId)].operation != Operation::OpenEntranceDoor)
            throw ConfigError("unsafe_constraint", "/compatibility", "Legacy operations changed");
        const auto& door = configuration.devices[catalog->devices_.at(mapping.doorTargetId)];
        if (door.kind != DeviceKind::EntranceDoor || door.actionIds != std::vector<std::string>{mapping.doorActionId})
            throw ConfigError("unsafe_constraint", "/compatibility", "Legacy door target changed kind or action");
        for (const auto& expected : legacyRooms()) {
            const auto target = mapping.lightTargets.at(expected.code);
            if (!catalog->devices_.count(target)) throw ConfigError("dangling_reference", "/compatibility/light_targets", "Unknown light target");
            const auto& light = configuration.devices[catalog->devices_.at(target)];
            if (target != expected.device || light.roomId != expected.room || light.kind != DeviceKind::Light || light.actionIds != std::vector<std::string>{mapping.lightActionId})
                throw ConfigError("legacy_incompatible", "/compatibility/light_targets/" + std::string(1, expected.code), "Legacy target identity/location cannot be remapped");
            const auto& room = configuration.rooms[catalog->rooms_.at(expected.room)];
            if (!sameStrings(room.mentions, expected.mentions) || !sameStrings(room.locations, expected.locations))
                catalog->legacyDifferences_.push_back("room_aliases:" + room.id);
        }
    } else {
        for (const auto& d : configuration.devices) if (d.kind != DeviceKind::Generic)
            throw ConfigError("legacy_required", "/compatibility", "Protected legacy devices require compatibility mapping");
        for (const auto& a : configuration.actions) if (a.operation != Operation::AssignState)
            throw ConfigError("legacy_required", "/compatibility", "Protected legacy actions require compatibility mapping");
    }
    if (configuration.rooms.size() != 4 || configuration.devices.size() != 5 || configuration.actions.size() != 2)
        catalog->legacyDifferences_.push_back("configured_entity_set");
    for (const auto& device : configuration.devices) for (const auto& id : device.actionIds) {
        const auto& action = configuration.actions[catalog->actions_.at(id)];
        EffectiveAction effective{device.id, id, action.operation, device.enabled && action.enabled && !denied.count({device.id, id}), std::nullopt, {}};
        if (action.operation == Operation::SetBrightness) {
            effective.brightnessBounds = std::make_pair(action.parameters[0].minimum, action.parameters[0].maximum);
            effective.mandatoryPolicies = {"original_room_required_v1", "zero_with_enable_forbidden_v1"};
            if (*effective.brightnessBounds != std::make_pair(std::int64_t{0}, std::int64_t{100}))
                catalog->legacyDifferences_.push_back("brightness_bounds:" + device.id);
        } else if (action.operation == Operation::OpenEntranceDoor) effective.mandatoryPolicies = {"explicit_entrance_open_required_v1"};
        else effective.mandatoryPolicies = {"original_target_and_action_required_v2", "virtual_assignments_only_v2"};
        if (!effective.permitted) catalog->legacyDifferences_.push_back("disabled_or_denied:" + device.id + ":" + id);
        catalog->effective_.push_back(std::move(effective));
    }
    catalog->configuration_ = std::move(configuration);
    catalog->canonical_ = detail::encode(catalog->configuration_).dump(4) + "\n";
    if (catalog->canonical_.size() > MaxConfigBytes) throw ConfigError("size_limit", "", "Canonical configuration exceeds 256 KiB");
    catalog->hash_ = detail::sha256(catalog->canonical_);
    return catalog;
}
std::string ValidatedCatalog::summary() const {
    using detail::Json;
    Json result{{"schema_version", configuration_.schemaVersion}, {"revision", configuration_.revision}, {"content_sha256", hash_},
        {"storage_valid", true}, {"production_activated", false}, {"activation_checks", "not_implemented"},
        {"matches_current_legacy_domain", matchesCurrentLegacyDomain()}, {"legacy_differences", legacyDifferences_},
        {"capability_contract", "m4_v1"}, {"language_profile", configuration_.system.interpretationProfile},
        {"rooms", Json::array()}, {"devices", Json::array()}, {"effective_actions", Json::array()}};
    for (const auto& room : configuration_.rooms)
        result["rooms"].push_back({{"id", room.id}, {"name", room.name}, {"mentions", room.mentions}, {"locations", room.locations}});
    for (const auto& device : configuration_.devices)
        result["devices"].push_back({{"id", device.id}, {"name", device.name}, {"kind", detail::kindName(device.kind)},
            {"room_id", device.roomId ? Json(*device.roomId) : Json(nullptr)}, {"enabled", device.enabled}, {"action_ids", device.actionIds}});
    for (const auto& effective : effective_) {
        Json item{{"target_id", effective.deviceId}, {"action_id", effective.actionId}, {"operation", detail::operationName(effective.operation)},
            {"permitted", effective.permitted}, {"mandatory_policies", effective.mandatoryPolicies}};
        if (effective.brightnessBounds) item["brightness"] = {{"type", "integer"}, {"minimum", effective.brightnessBounds->first},
            {"maximum", effective.brightnessBounds->second}, {"unit", "percent"}};
        result["effective_actions"].push_back(std::move(item));
    }
    return result.dump(4) + "\n";
}
}
