#include "firefly/execution/DeviceStateStore.h"
#include <algorithm>
#include <chrono>

namespace firefly::execution {
namespace {
SteadyClockMs defaultClock() {
    return [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    };
}
}

DeviceStateStore::DeviceStateStore(config::Catalog catalog, SteadyClockMs clock)
    : domain_(std::move(catalog)), clock_(clock ? std::move(clock) : defaultClock()) {
    for (const auto& device : domain_.snapshot()->configuration().devices) {
        if (!device.enabled) continue;
        initializeUnknown(device);
    }
}

void DeviceStateStore::initializeUnknown(const config::Device& device) {
    DeviceKnowledge entry;
    entry.targetId = device.id;
    entry.name = device.name;
    if (device.kind == config::DeviceKind::Light) {
        entry.properties["brightness"] = {};
    } else if (device.kind == config::DeviceKind::EntranceDoor) {
        entry.properties["position"] = {};
    } else if (device.kind == config::DeviceKind::Generic) {
        for (const auto& field : device.state) entry.properties[field.id] = {};
    }
    devices_.emplace(device.id, std::move(entry));
}

std::int64_t DeviceStateStore::nowMs() const { return clock_(); }

std::string DeviceStateStore::knowledgeName(StateKnowledge value) {
    switch (value) {
        case StateKnowledge::Unknown: return "unknown";
        case StateKnowledge::Predicted: return "predicted";
        case StateKnowledge::Confirmed: return "confirmed";
    }
    return "unknown";
}

std::string DeviceStateStore::connectivityName(Connectivity value) {
    switch (value) {
        case Connectivity::Unknown: return "unknown";
        case Connectivity::Reachable: return "reachable";
        case Connectivity::Unreachable: return "unreachable";
    }
    return "unknown";
}

std::int64_t DeviceStateStore::relevantStamp(const PropertyKnowledge& property) {
    if (property.knowledge == StateKnowledge::Unknown) return 0;
    // Predicted uses updatedAtMs; Confirmed uses observedAtMs. Take the max so a
    // later prediction after an older confirmation still blocks stale telemetry.
    return std::max(property.updatedAtMs, property.observedAtMs);
}

void DeviceStateStore::validateObservationValue(const config::Device& device, const std::string& key,
                                                const actions::Scalar& value) const {
    if (device.kind == config::DeviceKind::Generic) {
        const auto field = std::find_if(device.state.begin(), device.state.end(),
            [&](const auto& item) { return item.id == key; });
        if (field != device.state.end() && !config::accepts(*field, value))
            throw config::ConfigError("unsafe_constraint", "/observation/properties/" + key,
                "Observation value violates the property schema");
        return;
    }
    if (device.kind == config::DeviceKind::Light && key == "brightness") {
        if (!std::holds_alternative<std::int64_t>(value.data()))
            throw config::ConfigError("unsafe_constraint", "/observation/properties/brightness",
                "Light brightness observation must be an integer");
        const auto brightness = value.value();
        bool checked = false;
        for (const auto& id : device.actionIds) {
            const auto* effective = domain_.effectiveAction(device.id, id);
            if (!effective || !effective->brightnessBounds) continue;
            checked = true;
            if (brightness < effective->brightnessBounds->first ||
                brightness > effective->brightnessBounds->second)
                throw config::ConfigError("unsafe_constraint", "/observation/properties/brightness",
                    "Light brightness observation is outside the device domain");
        }
        if (!checked && (brightness < 0 || brightness > 100))
            throw config::ConfigError("unsafe_constraint", "/observation/properties/brightness",
                "Light brightness observation is outside the protocol domain 0..100");
        return;
    }
    if (device.kind == config::DeviceKind::EntranceDoor && key == "position") {
        const auto* text = std::get_if<std::string>(&value.data());
        if (!text || (*text != "OPEN" && *text != "CLOSED"))
            throw config::ConfigError("unsafe_constraint", "/observation/properties/position",
                "Entrance door position observation must be OPEN or CLOSED");
        return;
    }
}

std::vector<DeviceKnowledge> DeviceStateStore::devices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DeviceKnowledge> result;
    for (const auto& [_, device] : devices_) result.push_back(device);
    return result;
}

std::optional<DeviceKnowledge> DeviceStateStore::device(const std::string& targetId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = devices_.find(targetId);
    if (found == devices_.end()) return std::nullopt;
    return found->second;
}

void DeviceStateStore::applyPrediction(const std::string& targetId,
                                       const std::map<std::string, actions::Scalar>& expected,
                                       std::int64_t atMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = devices_.find(targetId);
    if (found == devices_.end()) return;
    for (const auto& [key, value] : expected) {
        auto& property = found->second.properties[key];
        property.value = value;
        property.knowledge = StateKnowledge::Predicted;
        property.updatedAtMs = atMs;
    }
}

void DeviceStateStore::applyObservation(const StateObservation& observation) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = devices_.find(observation.targetId);
    if (found == devices_.end())
        throw config::ConfigError("unknown_target", "/observation/target_id",
            "Observation target is not in the active generation");
    const auto* device = domain_.device(observation.targetId);
    if (!device)
        throw config::ConfigError("unknown_target", "/observation/target_id", "Observation target missing from domain");

    // Validate schema + ordering for every property before mutating any of them.
    for (const auto& [key, value] : observation.properties) {
        if (!found->second.properties.count(key))
            throw config::ConfigError("unknown_field", "/observation/properties/" + key,
                "Observation property is not declared for the device");
        validateObservationValue(*device, key, value);
        const auto& property = found->second.properties.at(key);
        const auto stamp = relevantStamp(property);
        if (property.knowledge != StateKnowledge::Unknown && observation.timestampMs < stamp)
            throw config::ConfigError("stale_observation", "/observation/properties/" + key,
                "Observation timestamp is older than existing property knowledge");
    }

    for (const auto& [key, value] : observation.properties) {
        auto& property = found->second.properties.at(key);
        property.value = value;
        property.knowledge = StateKnowledge::Confirmed;
        property.updatedAtMs = observation.timestampMs;
        property.observedAtMs = observation.timestampMs;
    }
    found->second.connectivity = Connectivity::Reachable;
    found->second.lastSuccessAtMs = observation.timestampMs;
}

void DeviceStateStore::markReachable(const std::string& targetId, std::int64_t atMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = devices_.find(targetId);
    if (found == devices_.end()) return;
    found->second.connectivity = Connectivity::Reachable;
    found->second.lastSuccessAtMs = atMs;
}

void DeviceStateStore::markUnreachable(const std::string& targetId, std::int64_t atMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = devices_.find(targetId);
    if (found == devices_.end()) return;
    found->second.connectivity = Connectivity::Unreachable;
    found->second.lastFailureAtMs = atMs;
}

void DeviceStateStore::invalidate(const std::string& targetId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = devices_.find(targetId);
    if (found == devices_.end()) return;
    for (auto& [key, property] : found->second.properties) { (void)key; property = {}; }
    found->second.connectivity = Connectivity::Unknown;
    found->second.lastSuccessAtMs = found->second.lastFailureAtMs = 0;
}

void DeviceStateStore::reconcile(const DeviceStateStore& previous) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto prior = previous.devices();
    auto next = devices_;
    for (const auto& old : prior) {
        auto found = next.find(old.targetId);
        if (found == next.end()) continue;
        if (found->second.properties.size() != old.properties.size())
            throw config::ConfigError("state_conflict", "/devices/" + old.targetId,
                "Retained production property set changed; Apply does not reset physical knowledge implicitly.");
        for (const auto& [key, property] : old.properties) {
            if (!found->second.properties.count(key))
                throw config::ConfigError("state_conflict", "/devices/" + old.targetId + "/state/" + key,
                    "Retained production property identity changed.");
            if (property.knowledge == StateKnowledge::Unknown || !property.value) continue;
            const auto* device = domain_.device(old.targetId);
            if (device && device->kind == config::DeviceKind::Generic) {
                const auto field = std::find_if(device->state.begin(), device->state.end(),
                    [&](const auto& item) { return item.id == key; });
                if (field != device->state.end() && !config::accepts(*field, *property.value))
                    throw config::ConfigError("state_conflict", "/devices/" + old.targetId + "/state/" + key,
                        "Retained production knowledge violates the proposed schema.");
            }
            if (device && device->kind == config::DeviceKind::Light && key == "brightness") {
                for (const auto& id : device->actionIds) {
                    const auto* effective = domain_.effectiveAction(old.targetId, id);
                    if (effective && effective->brightnessBounds) {
                        const auto brightness = property.value->value();
                        if (brightness < effective->brightnessBounds->first ||
                            brightness > effective->brightnessBounds->second)
                            throw config::ConfigError("state_conflict", "/devices/" + old.targetId,
                                "Current production brightness is outside the proposed range.");
                    }
                }
            }
            if (device && device->kind == config::DeviceKind::EntranceDoor && key == "position") {
                const auto* text = property.value ? std::get_if<std::string>(&property.value->data()) : nullptr;
                if (!text || (*text != "OPEN" && *text != "CLOSED"))
                    throw config::ConfigError("state_conflict", "/devices/" + old.targetId + "/state/position",
                        "Retained entrance door position is not OPEN or CLOSED.");
            }
            found->second.properties.at(key) = property;
        }
        found->second.connectivity = old.connectivity;
        found->second.lastSuccessAtMs = old.lastSuccessAtMs;
        found->second.lastFailureAtMs = old.lastFailureAtMs;
    }
    devices_ = std::move(next);
}
}
