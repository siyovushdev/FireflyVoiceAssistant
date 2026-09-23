#include "firefly/admin/AdminCopilot.h"
#include "firefly/admin/AdminDraft.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/config/ConfigValidator.h"
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
bool pinnedDevice(const Json& value, const Json& device) {
    const auto& compatibility = value.at("compatibility");
    if (device.at("id") == compatibility.at("door_target_id")) return true;
    for (const auto& item : compatibility.at("light_targets").items())
        if (item.value() == device.at("id")) return true;
    return false;
}
bool pinnedAction(const Json& value, const std::string& actionId) {
    const auto& compatibility = value.at("compatibility");
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
    if (kind == "create_location") {
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
        requireText(op.at("location"), 128); requireText(op.at("name"), 128); requireText(op.at("template"), 32);
        requireText(op.at("description"), 256);
        Json aliases = op.contains("aliases") ? op.at("aliases") : Json::array({detail::lowercase(op.at("name").get<std::string>())});
        requireStrings(aliases, 8);
        Json deviceLike = op;
        deviceLike["aliases"] = aliases;
        if (protectedEntranceDoorSpeech(deviceLike)) rejectProtectedEntranceDoor();
        const auto* location = resolveLocation(value, op.at("location").get<std::string>());
        const auto& selected = templateById(templates, op.at("template"));
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
            {"room_id", location->at("id")}, {"enabled", true}, {"action_ids", actionIds},
            {"spoken_aliases", aliases}, {"state", selected.at("state")},
            {"metadata", {{"type", selected.at("type")}, {"description", op.at("description")},
                {"aliases", aliases}, {"origin", "local_ai"}, {"confirmed", false}}}});
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
    } else unsupported("Unknown operation \"" + kind + "\"");
}
} // namespace

std::string draftFingerprint(config::Catalog draft) {
    return draft->contentHash();
}

std::string copilotPrompt(config::Catalog draft) {
    const auto value = migrated(draft);
    const auto scope = scopedInventory(value);
    return std::string(R"(Admin Copilot. Только JSON.
{"status":"ok|ambiguous|unsupported","summary":"…","operations":[…]}
≤8 ops; ≤2 create_location; ≤6 create_device.
create_location{name,type,description,mentions?,locations?}
create_device{location,name,template,description,aliases?}
rename_location{location,name,keep_previous_as_alias}
add_location_mention{location,mention}
add_location_phrase{location,phrase}
delete_device{device,location}
set_field_bounds{device,location,field,minimum,maximum}
Не угадывай. Не door/входную дверь как switch. Без ID/Save/Apply.
)") + scope;
}

std::string decodeCopilotProposal(std::string_view raw, config::Catalog draft) {
    if (raw.size() > 4096) throw std::invalid_argument("Copilot proposal too large");
    const auto baseHash = draftFingerprint(draft);
    const auto proposal = parseMessage(raw, true);
    members(proposal, {"status", "summary", "operations"});
    requireText(proposal.at("status"), 32);
    requireText(proposal.at("summary"), 320);
    if (!proposal.at("operations").is_array() || proposal.at("operations").size() > 8)
        throw std::invalid_argument("Copilot supports at most 8 operations");
    const auto status = proposal.at("status").get<std::string>();
    if (status == "ambiguous" || status == "unsupported") {
        if (!proposal.at("operations").empty()) throw std::invalid_argument("Non-ok status must have empty operations");
        return Json{{"rejected", true}, {"status", status}, {"summary", proposal.at("summary")},
            {"operations", Json::array()}, {"base_hash", baseHash}, {"saved", false}, {"applied", false}}.dump();
    }
    if (status != "ok") throw std::invalid_argument("Invalid copilot status");
    if (proposal.at("operations").empty()) throw std::invalid_argument("ok status requires operations");

    std::size_t createLocations = 0, createDevices = 0;
    for (const auto& op : proposal.at("operations")) {
        if (!op.is_object() || !op.contains("op")) throw std::invalid_argument("Invalid operation");
        const auto kind = op.at("op").get<std::string>();
        if (kind == "create_location") ++createLocations;
        if (kind == "create_device") ++createDevices;
    }
    if (createLocations > 2) unsupported("At most two new locations per Copilot proposal");
    if (createDevices > 6) unsupported("At most six new devices per Copilot proposal");

    auto value = migrated(draft);
    auto ids = occupied(value);
    const auto before = value.dump();
    const auto templates = Json::parse(templatesJson());
    for (const auto& op : proposal.at("operations")) applyOperation(value, ids, op, templates);

    const auto validated = config::ConfigLoader::fromJson(value.dump());
    const auto after = validated->canonicalJson();
    const auto changes = Json::parse(configurationDiff(before, after));
    if (changes.empty()) throw std::invalid_argument("Copilot proposal produced no configuration changes");
    return Json{{"configuration", Json::parse(after)}, {"changes", changes},
        {"summary", proposal.at("summary")}, {"operations", proposal.at("operations")},
        {"base_hash", baseHash}, {"saved", false}, {"applied", false}}.dump();
}

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
