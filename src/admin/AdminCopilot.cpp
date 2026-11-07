#include "firefly/admin/AdminCopilot.h"
#include "firefly/admin/AdminDraft.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/config/ConfigValidator.h"
#include "firefly/config/LanguageProfile.h"
#include "../Text.h"
#include "../config/Platform.h"
#include "Json.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace firefly::admin {
namespace {
Json migrated(config::Catalog candidate) {
    return Json::parse(config::ConfigValidator::validate(config::migrateV2(candidate->configuration()))->canonicalJson());
}
std::set<std::string> occupied(const Json& value) {
    std::set<std::string> ids = value.at("retired_ids").get<std::set<std::string>>();
    ids.insert(value.at("system").at("id").get<std::string>());
    for (const auto* key : {"rooms", "devices", "actions"})
        for (const auto& entity : value.at(key)) ids.insert(entity.at("id").get<std::string>());
    return ids;
}
std::string allocate(std::set<std::string>& ids, const char* category, const std::string& name) {
    if (std::string_view(category) == "connection") {
        auto base = std::string("connection_") + deterministicId("device", name).substr(7);
        if (base.size() > 56) base.resize(56);
        while (!base.empty() && base.back() == '_') base.pop_back();
        auto id = base;
        for (std::size_t suffix = 2; ids.count(id); ++suffix) id = base + "_" + std::to_string(suffix);
        ids.insert(id); return id;
    }
    const auto id = deterministicId(category, name, ids);
    ids.insert(id);
    return id;
}
void requireText(const Json& value, std::size_t maximum, bool empty = false) {
    if (empty && value.is_null()) return;
    if (!value.is_string() || value.get_ref<const std::string&>().size() > maximum ||
        (!empty && value.get_ref<const std::string&>().empty()))
        throw std::invalid_argument("Invalid copilot string");
}
void requireStrings(const Json& value, std::size_t maximum) {
    if (!value.is_array() || value.size() > maximum) throw std::invalid_argument("Invalid copilot list");
    std::set<std::string> distinct;
    for (const auto& item : value) {
        requireText(item, 96);
        if (!distinct.insert(item.get<std::string>()).second) throw std::invalid_argument("Duplicate copilot entry");
    }
}
const Json& templateById(const Json& templates, const Json& id) {
    std::string key = id.get<std::string>();
    if (key == "air_conditioner" || key == "ac" || key == "conditioner" || key == "климат") key = "switch";
    if (key == "lamp" || key == "lighting") key = "light";
    if (key == "television") key = "tv";
    for (const auto& item : templates) if (item.at("id") == key) return item;
    throw std::invalid_argument("Unknown mock template");
}
Json defaultMentions(const std::string& name) {
    return Json::array({detail::lowercase(name)});
}
Json defaultLocations(const std::string& name) {
    return Json::array({"в " + detail::lowercase(name)});
}
bool protectedEntranceDoorSpeech(const Json& deviceLike) {
    if (deviceLike.contains("template") && deviceLike.at("template") == "door") return true;
    std::string blob = deviceLike.contains("name") ? deviceLike.at("name").get<std::string>() : "";
    if (deviceLike.contains("aliases"))
        for (const auto& alias : deviceLike.at("aliases")) blob.append(" ").append(alias.get_ref<const std::string&>());
    if (deviceLike.contains("description")) blob.append(" ").append(deviceLike.at("description").get_ref<const std::string&>());
    const auto folded = detail::lowercase(blob);
    const bool doorStem = folded.find("двер") != std::string::npos;
    const bool entrance = folded.find("входн") != std::string::npos || folded.find("на входе") != std::string::npos;
    return doorStem && entrance;
}
[[noreturn]] void rejectProtectedEntranceDoor() {
    throw std::invalid_argument("Protected entrance door cannot be created or modified by Copilot; configure it separately");
}
[[noreturn]] void ambiguous(const std::string& reason) {
    throw std::invalid_argument("ambiguous: " + reason);
}
[[noreturn]] void unsupported(const std::string& reason) {
    throw std::invalid_argument("unsupported: " + reason);
}
struct CopilotRejection : std::runtime_error {
    std::string status;
    std::string code;
    explicit CopilotRejection(std::string status, std::string code, std::string summary)
        : std::runtime_error(summary), status(std::move(status)), code(std::move(code)) {}
};
[[noreturn]] void rejectProposal(std::string code, std::string summary) {
    throw CopilotRejection("unsupported", std::move(code), std::move(summary));
}
bool equalsFolded(std::string_view a, std::string_view b) { return detail::lowercase(a) == detail::lowercase(b); }
Json* resolveLocation(Json& value, const std::string& ref, bool required = true) {
    if (ref.empty()) {
        if (required) ambiguous("Location reference is empty");
        return nullptr;
    }
    std::vector<Json*> matches;
    for (auto& room : value.at("rooms")) {
        bool hit = equalsFolded(room.at("id").get<std::string>(), ref) || equalsFolded(room.at("name").get<std::string>(), ref);
        if (!hit) for (const auto& mention : room.at("speech").at("mentions"))
            if (equalsFolded(mention.get<std::string>(), ref)) { hit = true; break; }
        if (!hit) for (const auto& phrase : room.at("speech").at("locations"))
            if (equalsFolded(phrase.get<std::string>(), ref)) { hit = true; break; }
        if (hit) matches.push_back(&room);
    }
    if (matches.size() > 1) ambiguous("Multiple locations match \"" + ref + "\"");
    if (matches.empty()) {
        if (required) ambiguous("No location matches \"" + ref + "\"");
        return nullptr;
    }
    return matches.front();
}
Json* resolveDevice(Json& value, const std::string& deviceRef, const std::string& locationRef) {
    if (deviceRef.empty()) ambiguous("Device reference is empty");
    Json* location = locationRef.empty() ? nullptr : resolveLocation(value, locationRef, true);
    std::vector<Json*> matches;
    for (auto& device : value.at("devices")) {
        if (location && device.at("room_id") != location->at("id")) continue;
        bool hit = equalsFolded(device.at("id").get<std::string>(), deviceRef) ||
                   equalsFolded(device.at("name").get<std::string>(), deviceRef);
        if (!hit) for (const auto& alias : device.at("spoken_aliases"))
            if (equalsFolded(alias.get<std::string>(), deviceRef)) { hit = true; break; }
        if (!hit && device.contains("metadata"))
            for (const auto& alias : device.at("metadata").value("aliases", Json::array()))
                if (equalsFolded(alias.get<std::string>(), deviceRef)) { hit = true; break; }
        if (hit) matches.push_back(&device);
    }
    if (matches.size() > 1) ambiguous("Multiple devices match \"" + deviceRef + "\"");
    if (matches.empty()) ambiguous("No device matches \"" + deviceRef + "\"");
    return matches.front();
}
Json* resolveConnection(Json& value, const std::string& ref) {
    Json* selected = nullptr;
    for (auto& connection : value["connections"]) {
        if (equalsFolded(connection.at("id").get<std::string>(), ref) || equalsFolded(connection.at("name").get<std::string>(), ref)) {
            if (selected) ambiguous("Multiple connections match the requested name");
            selected = &connection;
        }
    }
    if (!selected) ambiguous("Connection was not defined in the draft or this plan");
    return selected;
}
Json* resolveDeviceAction(Json& value, const Json& device, const std::string& ref) {
    Json* selected = nullptr;
    for (auto& action : value["actions"]) {
        if (std::find(device.at("action_ids").begin(), device.at("action_ids").end(), action.at("id")) == device.at("action_ids").end()) continue;
        bool hit = equalsFolded(action.at("id").get<std::string>(), ref) || equalsFolded(action.at("name").get<std::string>(), ref);
        for (const auto& alias : action.value("speech_aliases", Json::array())) hit = hit || equalsFolded(alias.get<std::string>(), ref);
        if (!hit && (ref == "ON" || ref == "OFF")) {
            const auto assignments = action.value("assignments", Json::array());
            hit = assignments.size() == 1 && assignments[0].value("field", std::string()) == "power" &&
                assignments[0].contains("value") && assignments[0]["value"].is_boolean() &&
                assignments[0]["value"].get<bool>() == (ref == "ON");
        }
        if (hit) { if (selected) ambiguous("Multiple actions match on this device"); selected = &action; }
    }
    if (!selected) ambiguous("Action is not available on the selected device");
    return selected;
}
bool pinnedDevice(const Json& value, const Json& device) {
    const auto& compatibility = value.at("compatibility");
    if (compatibility.is_null()) return false;
    if (device.at("id") == compatibility.at("door_target_id")) return true;
    for (const auto& item : compatibility.at("light_targets").items())
        if (item.value() == device.at("id")) return true;
    return false;
}
bool pinnedAction(const Json& value, const std::string& actionId) {
    const auto& compatibility = value.at("compatibility");
    if (compatibility.is_null()) return false;
    return actionId == compatibility.at("light_action_id").get<std::string>() ||
           actionId == compatibility.at("door_action_id").get<std::string>();
}
void removeExclusiveActions(Json& value, const Json& device) {
    for (const auto& actionId : device.at("action_ids")) {
        const auto id = actionId.get<std::string>();
        if (pinnedAction(value, id)) continue;
        bool shared = false;
        for (const auto& other : value.at("devices")) {
            if (other.at("id") == device.at("id")) continue;
            for (const auto& bound : other.at("action_ids")) if (bound == id) { shared = true; break; }
            if (shared) break;
        }
        if (shared) continue;
        auto& actions = value.at("actions");
        actions.erase(std::remove_if(actions.begin(), actions.end(), [&](const Json& a) { return a.at("id") == id; }), actions.end());
    }
}
std::string scopedInventory(const Json& value) {
    std::string out = "DRAFT:\nL:";
    for (const auto& room : value.at("rooms")) {
        out.append("\n").append(room.at("name").get<std::string>());
        out.append("|").append(room.at("speech").at("mentions").dump());
        out.append("|").append(room.at("speech").at("locations").dump());
    }
    out.append("\nD:");
    for (const auto& device : value.at("devices")) {
        std::string roomName = "-";
        if (!device.at("room_id").is_null()) {
            roomName = device.at("room_id").get<std::string>();
            for (const auto& room : value.at("rooms"))
                if (room.at("id") == device.at("room_id")) { roomName = room.at("name").get<std::string>(); break; }
        }
        out.append("\n").append(device.at("name").get<std::string>());
        out.append("@").append(roomName);
        out.append("/").append(device.at("kind").get<std::string>());
        if (device.contains("state") && device.at("state").is_array())
            for (const auto& field : device.at("state")) {
                if (!field.contains("id")) continue;
                out.append(" ").append(field.at("id").get<std::string>());
                if (field.contains("minimum") && field.contains("maximum")) {
                    const auto formatBound = [](const Json& n) {
                        if (n.is_number_integer()) return std::to_string(n.get<std::int64_t>());
                        const auto value = n.get<double>();
                        if (std::isfinite(value) && value == static_cast<double>(static_cast<std::int64_t>(value)) &&
                            value >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
                            value <= static_cast<double>(std::numeric_limits<std::int64_t>::max()))
                            return std::to_string(static_cast<std::int64_t>(value));
                        return n.dump();
                    };
                    out.append(":").append(formatBound(field.at("minimum")))
                        .append("..").append(formatBound(field.at("maximum")));
                }
            }
    }
    out.append("\nT:light,tv,switch,conveyor,numeric,enum,custom; never door");
    if (out.size() > 900)
        throw config::ConfigError("context_overflow", "/configuration",
            "Draft inventory exceeds Copilot context budget; narrow the request or reduce draft size.");
    return out;
}
void applyOperation(Json& value, std::set<std::string>& ids, const Json& op, const Json& templates) {
    requireText(op.at("op"), 64);
    const auto kind = op.at("op").get<std::string>();
    if (kind == "rename_system") {
        members(op, {"op", "name"}); requireText(op.at("name"), 128);
        value["system"]["name"] = op.at("name");
    } else if (kind == "create_location") {
        for (const auto* key : {"op", "name", "type", "description"})
            if (!op.contains(key)) throw std::invalid_argument(std::string("Missing field ") + key);
        requireText(op.at("name"), 128); requireText(op.at("type"), 64); requireText(op.at("description"), 256);
        Json mentions = op.contains("mentions") ? op.at("mentions") : defaultMentions(op.at("name").get<std::string>());
        Json locations = op.contains("locations") ? op.at("locations") : defaultLocations(op.at("name").get<std::string>());
        requireStrings(mentions, 8); requireStrings(locations, 8);
        const auto locationId = allocate(ids, "location", op.at("name").get<std::string>());
        value["rooms"].push_back({{"id", locationId}, {"name", op.at("name")},
            {"speech", {{"mentions", mentions}, {"locations", locations}}},
            {"metadata", {{"type", op.at("type")}, {"description", op.at("description")}, {"aliases", Json::array()},
                {"parent_id", nullptr}, {"container", false}, {"origin", "local_ai"}, {"confirmed", false}}}});
    } else if (kind == "create_device") {
        for (const auto* key : {"op", "location", "name", "template", "description"})
            if (!op.contains(key)) throw std::invalid_argument(std::string("Missing field ") + key);
        if (!op.at("location").is_null()) requireText(op.at("location"), 128);
        requireText(op.at("name"), 128); requireText(op.at("template"), 32);
        requireText(op.at("description"), 256);
        Json aliases = op.contains("aliases") ? op.at("aliases") : Json::array({detail::lowercase(op.at("name").get<std::string>())});
        requireStrings(aliases, 8);
        Json deviceLike = op;
        deviceLike["aliases"] = aliases;
        if (protectedEntranceDoorSpeech(deviceLike)) rejectProtectedEntranceDoor();
        const bool noLocation = op.at("location").is_null();
        const auto locationRef = noLocation ? std::string{} : op.at("location").get<std::string>();
        const auto* location = noLocation ? nullptr : resolveLocation(value, locationRef, false);
        if (!noLocation && !location) {
            rejectProposal("unknown_location",
                "Зона «" + locationRef + "» не найдена в черновике. Сначала явно создайте её "
                "(create_location), затем добавьте устройство.");
        }
        const auto& selected = templateById(templates, value.at("compatibility").is_null() && op.at("template") == "light" ? Json("generic_light") : op.at("template"));
        const auto name = op.at("name").get<std::string>();
        const auto id = allocate(ids, "device", name);
        const bool light = selected.at("id") == "light";
        Json actionIds = Json::array();
        if (light) actionIds.push_back(value.at("compatibility").at("light_action_id"));
        for (auto action : selected.at("actions")) {
            action["origin"] = "local_ai";
            action["id"] = allocate(ids, "action", name + " " + action.at("name").get<std::string>());
            actionIds.push_back(action.at("id"));
            value["actions"].push_back(action);
        }
        value["devices"].push_back({{"id", id}, {"name", name}, {"kind", light ? "light" : "generic"},
            {"room_id", location ? location->at("id") : Json(nullptr)}, {"enabled", true}, {"action_ids", actionIds},
            {"spoken_aliases", aliases}, {"state", selected.at("state")},
            {"metadata", {{"type", selected.at("type")}, {"description", op.at("description")},
                {"aliases", aliases}, {"origin", "local_ai"}, {"confirmed", false}}}});
    } else if (kind == "add_brightness") {
        members(op,{"op","device","location"});
        requireText(op.at("device"),128);requireText(op.at("location"),128);
        auto* device=resolveDevice(value,op.at("device").get<std::string>(),op.at("location").get<std::string>());
        if(pinnedDevice(value,*device)||device->at("kind")!="generic") unsupported("Protected or non-generic device");
        for(const auto& field:device->at("state")) if(field.at("id")=="level") unsupported("Brightness already exists");
        const auto& light=templateById(templates,"generic_light");
        device->at("state").push_back(light.at("state").at(0));
        auto action=light.at("actions").at(2);
        action["id"]=allocate(ids,"action",device->at("name").get<std::string>()+" яркость");
        device->at("action_ids").push_back(action.at("id"));
        value.at("actions").push_back(action);
    } else if (kind == "rename_device") {
        members(op, {"op", "device", "location", "name"});
        requireText(op.at("device"),128); requireText(op.at("location"),128);
        requireText(op.at("name"),128);
        auto* device = resolveDevice(value,op.at("device").get<std::string>(),op.at("location").get<std::string>());
        if (pinnedDevice(value,*device)) unsupported("Protected device cannot be renamed by dialogue");
        device->at("name") = op.at("name");
        // The explicitly requested new name is also command language. Preserve
        // reviewed aliases, and validate the added atom with the existing rules.
        const auto alias = config::normalizeLanguageAtom(op.at("name").get<std::string>());
        config::validateLanguageAtom(alias);
        auto& aliases = device->at("spoken_aliases");
        if (std::find(aliases.begin(), aliases.end(), alias) == aliases.end()) aliases.push_back(alias);
    } else if (kind == "rename_location") {
        members(op, {"op", "location", "name", "keep_previous_as_alias"});
        requireText(op.at("location"), 128); requireText(op.at("name"), 128);
        if (!op.at("keep_previous_as_alias").is_boolean()) throw std::invalid_argument("keep_previous_as_alias must be boolean");
        auto* location = resolveLocation(value, op.at("location").get<std::string>());
        const auto previous = location->at("name").get<std::string>();
        location->at("name") = op.at("name");
        if (op.at("keep_previous_as_alias").get<bool>()) {
            auto& mentions = location->at("speech").at("mentions");
            bool present = false;
            for (const auto& mention : mentions) if (equalsFolded(mention.get<std::string>(), previous)) present = true;
            if (!present) mentions.push_back(detail::lowercase(previous));
        }
    } else if (kind == "add_location_mention") {
        members(op, {"op", "location", "mention"});
        requireText(op.at("location"), 128); requireText(op.at("mention"), 96);
        auto* location = resolveLocation(value, op.at("location").get<std::string>());
        auto& mentions = location->at("speech").at("mentions");
        for (const auto& mention : mentions)
            if (equalsFolded(mention.get<std::string>(), op.at("mention").get<std::string>())) return;
        mentions.push_back(op.at("mention"));
    } else if (kind == "add_location_phrase") {
        members(op, {"op", "location", "phrase"});
        requireText(op.at("location"), 128); requireText(op.at("phrase"), 96);
        auto* location = resolveLocation(value, op.at("location").get<std::string>());
        auto& phrases = location->at("speech").at("locations");
        for (const auto& phrase : phrases)
            if (equalsFolded(phrase.get<std::string>(), op.at("phrase").get<std::string>())) return;
        phrases.push_back(op.at("phrase"));
    } else if (kind == "delete_device") {
        members(op, {"op", "device", "location"});
        requireText(op.at("device"), 128); requireText(op.at("location"), 128, true);
        auto* device = resolveDevice(value, op.at("device").get<std::string>(),
            op.at("location").is_null() ? "" : op.at("location").get<std::string>());
        if (pinnedDevice(value, *device)) unsupported("Protected or system device cannot be deleted by Copilot");
        if (device->at("kind") == "entrance_door") unsupported("Protected entrance door cannot be deleted by Copilot");
        for (const auto* key : {"bindings", "observations"}) if (value.contains(key)) {
            auto& list = value.at(key);
            list.erase(std::remove_if(list.begin(), list.end(), [&](const auto& b) { return b.at("target_id") == device->at("id"); }), list.end());
        }
        removeExclusiveActions(value, *device);
        auto& devices = value.at("devices");
        devices.erase(std::remove_if(devices.begin(), devices.end(), [&](const Json& d) { return d.at("id") == device->at("id"); }), devices.end());
    } else if (kind == "set_field_bounds") {
        members(op, {"op", "device", "location", "field", "minimum", "maximum"});
        requireText(op.at("device"), 128); requireText(op.at("location"), 128, true); requireText(op.at("field"), 64);
        auto* device = resolveDevice(value, op.at("device").get<std::string>(),
            op.at("location").is_null() ? "" : op.at("location").get<std::string>());
        if (device->at("kind") != "generic") unsupported("Field bounds apply only to generic devices");
        Json* field = nullptr;
        for (auto& item : device->at("state")) if (item.at("id") == op.at("field")) { field = &item; break; }
        if (!field) ambiguous("No state field \"" + op.at("field").get<std::string>() + "\" on device");
        const auto type = field->at("type").get<std::string>();
        if (type != "integer" && type != "number") unsupported("Bounds apply only to numeric fields");
        Json minimum = op.at("minimum"), maximum = op.at("maximum");
        if (type == "integer") {
            if (!minimum.is_number_integer() || !maximum.is_number_integer())
                throw std::invalid_argument("Integer field bounds must be JSON integers");
            const auto lo = minimum.get<std::int64_t>(), hi = maximum.get<std::int64_t>();
            if (lo > hi) throw std::invalid_argument("Minimum exceeds maximum");
            (*field)["minimum"] = lo;
            (*field)["maximum"] = hi;
            if (field->contains("initial") && field->at("initial").is_number_integer()) {
                auto initial = field->at("initial").get<std::int64_t>();
                if (initial < lo) (*field)["initial"] = lo;
                if (initial > hi) (*field)["initial"] = hi;
            }
        } else {
            if (!minimum.is_number() || !maximum.is_number())
                throw std::invalid_argument("Number field bounds must be JSON numbers");
            const auto lo = minimum.get<double>(), hi = maximum.get<double>();
            if (!std::isfinite(lo) || !std::isfinite(hi)) throw std::invalid_argument("Number bounds must be finite");
            if (lo > hi) throw std::invalid_argument("Minimum exceeds maximum");
            (*field)["minimum"] = lo;
            (*field)["maximum"] = hi;
            if (field->contains("initial") && field->at("initial").is_number()) {
                auto initial = field->at("initial").get<double>();
                if (initial < lo) (*field)["initial"] = lo;
                if (initial > hi) (*field)["initial"] = hi;
            }
        }
        for (const auto& actionId : device->at("action_ids")) {
            for (auto& action : value.at("actions")) {
                if (action.at("id") != actionId) continue;
                for (auto& parameter : action.at("parameters"))
                    if (parameter.at("id") == op.at("field")) {
                        parameter["minimum"] = (*field)["minimum"];
                        parameter["maximum"] = (*field)["maximum"];
                    }
            }
        }
    } else if (kind == "setup_connection") {
        members(op, {"op", "name", "settings"}); requireText(op.at("name"), 128);
        const auto& settings = op.at("settings");
        if (!settings.is_object() || !settings.contains("type")) unsupported("Connection type required");
        for (const auto& member : settings.items())
            if (!std::set<std::string>{"type", "timeout_ms", "http", "network", "mqtt", "serial", "modbus"}.count(member.key()))
                unsupported("Unknown connection setting");
        if (!value.contains("connections")) value["connections"] = Json::array();
        Json* existing = nullptr;
        for (auto& connection : value["connections"]) if (equalsFolded(connection.at("name").get<std::string>(), op.at("name").get<std::string>())) {
            if (existing) ambiguous("Multiple connections share that name"); existing = &connection;
        }
        // Partial AI edits preserve private references and unmentioned options.
        // A protocol change needs the explicit manual transport workflow.
        if (existing && existing->at("type") != settings.at("type"))
            unsupported("Change an existing connection's protocol using the manual connection editor");
        Json entry = existing ? *existing : Json{{"enabled",true},{"timeout_ms",2000}};
        entry.merge_patch(settings); entry["name"] = op.at("name");
        if (existing) { entry["id"] = existing->at("id"); *existing = entry; }
        else {
            for (const auto& c : value["connections"]) ids.insert(c.at("id").get<std::string>());
            entry["id"] = allocate(ids, "connection", op.at("name")); value["connections"].push_back(entry);
        }
    } else if (kind == "setup_binding" || kind == "setup_observe") {
        members(op, kind == "setup_binding" ? std::set<std::string>{"op", "location", "device", "action", "connection", "settings"} :
            std::set<std::string>{"op", "location", "device", "connection", "settings"});
        requireText(op.at("device"), 128); requireText(op.at("location"), 128, true); requireText(op.at("connection"), 128);
        auto* device = resolveDevice(value, op.at("device"), op.at("location").is_null() ? "" : op.at("location").get<std::string>());
        auto* connection = resolveConnection(value, op.at("connection"));
        const auto& settings = op.at("settings"); if (!settings.is_object()) unsupported("Typed mapping required");
        for (const auto& member : settings.items())
            if (!std::set<std::string>{"http", "mqtt", "modbus", "fields"}.count(member.key())) unsupported("Unknown mapping setting");
        Json entry = settings; entry["target_id"] = device->at("id"); entry["connection_id"] = connection->at("id"); entry["executor"] = connection->at("type");
        const auto category = kind == "setup_binding" ? "bindings" : "observations";
        if (kind == "setup_binding") { requireText(op.at("action"), 128); entry["action_id"] = resolveDeviceAction(value, *device, op.at("action"))->at("id"); }
        if (!value.contains(category)) value[category] = Json::array();
        auto& list = value[category]; const auto found = std::find_if(list.begin(), list.end(), [&](const Json& old) {
            return old.at("target_id") == entry.at("target_id") && (kind != "setup_binding" || old.at("action_id") == entry.at("action_id"));
        });
        if (found == list.end()) list.push_back(entry); else *found = entry;
    } else if (kind == "set_http_connection") {
        members(op, {"op", "connection"});
        const auto& entry = op.at("connection");
        if (!entry.is_object() || entry.value("type", "") != "http") unsupported("Only HTTP connections are implemented");
        if (!value.contains("connections")) value["connections"] = Json::array();
        auto& list = value.at("connections");
        const auto id = entry.at("id");
        const auto found = std::find_if(list.begin(), list.end(), [&](const auto& c) { return c.at("id") == id; });
        if (found == list.end()) list.push_back(entry); else *found = entry;
    } else if (kind == "set_connection_binding") {
        members(op, {"op", "binding"});
        const auto& entry = op.at("binding");
        if (!entry.is_object() || !entry.contains("connection_id")) unsupported("Connection ID required");
        auto& list = value.at("bindings");
        const auto found = std::find_if(list.begin(), list.end(), [&](const auto& b) {
            return b.at("target_id") == entry.at("target_id") && b.at("action_id") == entry.at("action_id");
        });
        if (found == list.end()) list.push_back(entry); else *found = entry;
    } else if (kind == "set_http_observe") {
        members(op, {"op", "observation"});
        const auto& entry = op.at("observation");
        if (!entry.is_object()) unsupported("Observation object required");
        if (!value.contains("observations")) value["observations"] = Json::array();
        auto& list = value.at("observations");
        const auto found = std::find_if(list.begin(), list.end(), [&](const auto& o) { return o.at("target_id") == entry.at("target_id"); });
        if (found == list.end()) list.push_back(entry); else *found = entry;
    } else if (kind == "set_http_binding") {
        const std::set<std::string> allowed{"op","device","location","action","method","url","timeout_ms","content_type","body","argument"};
        for (const auto& item : op.items()) if (!allowed.count(item.key())) throw std::invalid_argument("Unknown binding operation field");
        // Explicit hardware connection only — never invent IP/path/protocol.
        for (const auto* key : {"op", "device", "location", "action", "method", "url"})
            if (!op.contains(key)) throw std::invalid_argument(std::string("Missing field ") + key);
        requireText(op.at("device"), 128); requireText(op.at("location"), 128, true);
        requireText(op.at("action"), 128); requireText(op.at("method"), 16);
        requireText(op.at("url"), 512, true);
        auto* device = resolveDevice(value, op.at("device").get<std::string>(),
            op.at("location").is_null() ? "" : op.at("location").get<std::string>());
        const auto actionRef = op.at("action").get<std::string>();
        Json* action = nullptr;
        for (auto& item : value.at("actions")) {
            if (equalsFolded(item.at("id").get<std::string>(), actionRef) ||
                equalsFolded(item.at("name").get<std::string>(), actionRef)) {
                if (action) ambiguous("Multiple actions match \"" + actionRef + "\"");
                action = &item;
            }
        }
        if (!action) ambiguous("No action matches \"" + actionRef + "\"");
        bool onDevice = false;
        for (const auto& id : device->at("action_ids"))
            if (id == action->at("id")) onDevice = true;
        if (!onDevice) unsupported("Action is not available on the selected device");
        const auto method = op.at("method").get<std::string>();
        if (method != "POST" && method != "PUT")
            unsupported("HTTP binding supports POST or PUT only");
        const auto url = op.at("url").is_null() ? std::string{} : op.at("url").get<std::string>();
        if (url.empty() || url.rfind("http://", 0) != 0)
            rejectProposal("incomplete_connection",
                "Укажите полный URL вида http://IPv4/путь. IP, порт и путь не выдумываются.");
        std::int64_t timeout = 1500;
        if (op.contains("timeout_ms")) {
            if (!op.at("timeout_ms").is_number_integer())
                throw std::invalid_argument("timeout_ms must be integer");
            timeout = op.at("timeout_ms").get<std::int64_t>();
        }
        std::string contentType = "application/json";
        if (op.contains("content_type")) {
            requireText(op.at("content_type"), 128);
            contentType = op.at("content_type").get<std::string>();
        }
        Json body = Json::object();
        if (op.contains("body")) {
            if (!op.at("body").is_object()) throw std::invalid_argument("HTTP body must be an object");
            body = op.at("body");
        }
        Json binding{{"target_id", device->at("id")}, {"action_id", action->at("id")}, {"executor", "http"},
            {"http", {{"method", method}, {"url", url}, {"timeout_ms", timeout},
                {"content_type", contentType}, {"body", body}}}};
        if (op.contains("argument") && !op.at("argument").is_null()) {
            requireText(op.at("argument"), 64);
            binding["http"]["argument"] = op.at("argument");
        }
        if (!value.contains("bindings") || !value.at("bindings").is_array()) value["bindings"] = Json::array();
        auto& bindings = value.at("bindings");
        bool replaced = false;
        for (auto& existing : bindings) {
            if (existing.at("target_id") == binding.at("target_id") &&
                existing.at("action_id") == binding.at("action_id")) {
                existing = binding;
                replaced = true;
                break;
            }
        }
        if (!replaced) bindings.push_back(binding);
    } else if (kind == "clear_http_binding") {
        members(op, {"op", "device", "location", "action"});
        requireText(op.at("device"), 128); requireText(op.at("location"), 128, true); requireText(op.at("action"), 128);
        auto* device = resolveDevice(value, op.at("device").get<std::string>(),
            op.at("location").is_null() ? "" : op.at("location").get<std::string>());
        const auto actionRef = op.at("action").get<std::string>();
        std::string actionId;
        for (auto& item : value.at("actions")) {
            if (equalsFolded(item.at("id").get<std::string>(), actionRef) ||
                equalsFolded(item.at("name").get<std::string>(), actionRef)) {
                if (!actionId.empty()) ambiguous("Multiple actions match \"" + actionRef + "\"");
                actionId = item.at("id").get<std::string>();
            }
        }
        if (actionId.empty()) ambiguous("No action matches \"" + actionRef + "\"");
        if (!value.contains("bindings") || !value.at("bindings").is_array()) return;
        auto& bindings = value.at("bindings");
        bindings.erase(std::remove_if(bindings.begin(), bindings.end(), [&](const Json& b) {
            return b.at("target_id") == device->at("id") && b.at("action_id") == actionId;
        }), bindings.end());
    } else unsupported("Unknown operation \"" + kind + "\"");
}
} // namespace

std::string draftFingerprint(config::Catalog draft) {
    return draft->contentHash();
}

std::string copilotPrompt(config::Catalog draft) {
    const auto value = migrated(draft);
    auto scope = scopedInventory(value);
    Json physical = {{"devices",Json::array()},{"connections",value.value("connections",Json::array())}};
    for (const auto& device : value.at("devices")) {
        Json actions = Json::array();
        for (const auto& id : device.at("action_ids")) for (const auto& action : value.at("actions"))
            if (action.at("id") == id) actions.push_back({{"id",id},{"name",action.at("name")},{"parameters",action.at("parameters")}});
        physical["devices"].push_back({{"id",device.at("id")},{"name",device.at("name")},
            {"actions",actions},{"state",device.value("state",Json::array())}});
    }
    scope += "\nBinding IDs (existing capabilities only):" + physical.dump();
    return std::string(R"(Admin Copilot. Только JSON.
{"status":"ok|ambiguous|unsupported","summary":"…","operations":[{"op":"…"}]}
Каждый element operations — плоский объект с discriminator "op" (строка). Пример:
{"op":"create_device","location":"Гостиная","name":"Телевизор","template":"tv","description":"ТВ.","aliases":["телевизор"]}
≠ {"create_device":{…}}. ≤8 ops; ≤2 create_location; ≤6 create_device.
rename_system{op,name} — только отображаемое название существующей Системы.
create_location{op,name,type,description,mentions?,locations?}
create_device{op,location,name,template,description,aliases?}
rename_location{op,location,name,keep_previous_as_alias}
rename_device{op,device,location,name}
add_brightness{op,device,location} — только по явной просьбе добавить яркость 0..100.
add_location_mention{op,location,mention}
add_location_phrase{op,location,phrase}
delete_device{op,device,location}
set_field_bounds{op,device,location,field,minimum,maximum}
set_http_connection{op,connection:{id,name,type:"http",enabled:true,timeout_ms:2000,http:{base_url,base_path:""}}}
set_connection_binding{op,binding:{target_id,action_id,connection_id,executor:"http",http:{method,path,body,expected_status:200}}}
set_http_observe{op,observation:{target_id,connection_id,executor:"http",http:{method:"GET",path,expected_status:200},fields:{response_field:existing_state_id}}}
В настройке подключения используй только эти три операции. Не меняй capabilities, не создавай устройства/действия; используй существующие IDs. Никогда не выполняй запросы. Не выдумывай адрес, путь, JSON contract или поле состояния. Проверку и Apply делает человек.
set_http_binding{op,device,location,action,method,url,timeout_ms?,content_type?,body?,argument?}
clear_http_binding{op,device,location,action}
Для явно запрошенного только включения/выключения используй template power_only (не tv/light).
create_device не меняет speech зоны. add_location_* только если пользователь просил речь.
location:null только при явном «без зоны». Не угадывай location. Не door/входную дверь как switch. Не выполняй Save/Apply.
HTTP-подключение: только из явно указанных пользователем method+URL (http://IPv4/…).
Не выдумывай IP, порт, path, protocol, credentials, GPIO, RS-485, payload.
Если данных мало → status unsupported/ambiguous и пустые operations.
)") + scope;
}

static std::string decodeCopilot(std::string_view raw, config::Catalog draft, bool document) {
    if (raw.size() > (document ? 16384 : 4096)) throw std::invalid_argument("Copilot proposal too large");
    const auto baseHash = draftFingerprint(draft);
    const auto proposal = parseMessage(raw, true);
    members(proposal, {"status", "summary", "operations"});
    requireText(proposal.at("status"), 32);
    requireText(proposal.at("summary"), 320);
    if (!proposal.at("operations").is_array() || proposal.at("operations").size() > (document ? 12 : 8))
        throw std::invalid_argument("Copilot operation limit exceeded");
    const auto status = proposal.at("status").get<std::string>();
    if (status == "ambiguous" || status == "unsupported") {
        if (!proposal.at("operations").empty()) throw std::invalid_argument("Non-ok status must have empty operations");
        return Json{{"rejected", true}, {"status", status}, {"summary", proposal.at("summary")},
            {"operations", Json::array()}, {"base_hash", baseHash}, {"saved", false}, {"applied", false}}.dump();
    }
    if (status != "ok") throw std::invalid_argument("Invalid copilot status");
    if (proposal.at("operations").empty()) throw std::invalid_argument("ok status requires operations");

    std::size_t createLocations = 0, createDevices = 0;
    bool speechEdit = false, connectivity = false, semantic = false;
    for (const auto& op : proposal.at("operations")) {
        if (!op.is_object() || !op.contains("op") || !op.at("op").is_string())
            throw std::invalid_argument("Invalid operation: each item needs string discriminator \"op\"");
        const auto kind = op.at("op").get<std::string>();
        if (kind.rfind("setup_", 0) == 0 && !document) throw std::invalid_argument("Whole-document grounding required for this operation");
        const bool physical = kind == "set_http_connection" || kind == "set_connection_binding" || kind == "set_http_observe" || kind == "set_http_binding" || kind == "clear_http_binding";
        connectivity = connectivity || physical; semantic = semantic || !physical;
        if (kind == "create_location") ++createLocations;
        if (kind == "create_device") ++createDevices;
        if (kind == "add_location_mention" || kind == "add_location_phrase") speechEdit = true;
    }
    if (connectivity && semantic && !document) throw std::invalid_argument("Connection proposals cannot change capabilities or semantic entities");
    if (createLocations > (document ? 6 : 2)) unsupported("Too many new locations in one proposal");
    if (createDevices > (document ? 10 : 6)) unsupported("Too many new devices in one proposal");
    if (createDevices > 0 && speechEdit)
        return Json{{"rejected", true}, {"status", "unsupported"}, {"code", "unsupported"},
            {"summary", "Добавление устройства не должно менять speech зоны. Уберите add_location_mention/phrase."},
            {"operations", Json::array()}, {"base_hash", baseHash}, {"saved", false}, {"applied", false}}.dump();

    auto value = migrated(draft);
    auto ids = occupied(value);
    const auto before = value.dump();
    const auto templates = Json::parse(templatesJson());
    try {
        for (const auto& op : proposal.at("operations")) applyOperation(value, ids, op, templates);
    } catch (const CopilotRejection& rejection) {
        return Json{{"rejected", true}, {"status", rejection.status}, {"code", rejection.code},
            {"summary", rejection.what()}, {"operations", Json::array()},
            {"base_hash", baseHash}, {"saved", false}, {"applied", false}}.dump();
    }

    const auto validated = config::ConfigLoader::fromJson(value.dump());
    const auto after = validated->canonicalJson();
    const auto changes = Json::parse(configurationDiff(before, after));
    if (changes.empty()) throw std::invalid_argument("Copilot proposal produced no configuration changes");
    return Json{{"configuration", Json::parse(after)}, {"changes", changes},
        {"summary", proposal.at("summary")}, {"operations", proposal.at("operations")},
        {"base_hash", baseHash}, {"saved", false}, {"applied", false}}.dump();
}
std::string decodeCopilotProposal(std::string_view raw, config::Catalog draft) { return decodeCopilot(raw, std::move(draft), false); }
std::string decodeSetupCopilotProposal(std::string_view raw, config::Catalog draft) { return decodeCopilot(raw, std::move(draft), true); }

std::string acceptCopilotProposal(std::string_view currentDraftJson, std::string_view proposalJson) {
    const auto current = config::ConfigLoader::fromJson(std::string(currentDraftJson));
    const auto proposal = parseMessage(proposalJson, true);
    for (const auto* key : {"configuration", "base_hash", "saved", "applied"})
        if (!proposal.contains(key)) throw std::invalid_argument(std::string("Missing field ") + key);
    if (proposal.value("saved", true) || proposal.value("applied", true))
        throw std::invalid_argument("Copilot accept refuses Save/Apply markers");
    requireText(proposal.at("base_hash"), 128);
    if (draftFingerprint(current) != proposal.at("base_hash").get<std::string>())
        throw config::ConfigError("stale_proposal", "/draft",
            "Copilot proposal is stale: draft changed since the proposal was created. Regenerate.");
    const auto accepted = config::ConfigLoader::fromJson(proposal.at("configuration").dump());
    return Json{{"configuration", Json::parse(accepted->canonicalJson())},
        {"saved", false}, {"applied", false}, {"ok", true}}.dump();
}
}
