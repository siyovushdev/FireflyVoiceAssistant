#include "firefly/execution/DeviceStateStore.h"
#include <algorithm>
#include <chrono>

namespace firefly::execution {
namespace {
SteadyClockMs defaultSteadyClock() {
    return [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };
}
}

DeviceStateStore::DeviceStateStore(config::Catalog catalog, SteadyClockMs steadyClock, WallClockMs wallClock)
    : domain_(std::move(catalog)), steadyClock_(steadyClock ? std::move(steadyClock) : defaultSteadyClock()),
      wallClock_(wallClock ? std::move(wallClock) : [] {
          return std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch()).count();
      }) {
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

std::int64_t DeviceStateStore::nowMs() const { return wallClock_(); }
std::int64_t DeviceStateStore::steadyNowMs() const { return steadyClock_(); }

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
                                       std::int64_t atMs, std::optional<std::int64_t> startedSteadyMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = devices_.find(targetId);
    if (found == devices_.end()) return;
    const auto steady = steadyNowMs();
    const auto order = startedSteadyMs.value_or(steady);
    for (const auto& [key, value] : expected) {
        auto& property = found->second.properties[key];
        // A subscribed state message may confirm the command while publish()
        // is still returning. Never overwrite that newer confirmed evidence.
        if (property.knowledge == StateKnowledge::Confirmed && property.orderSteadyMs &&
            *property.orderSteadyMs > order) continue;
        property.value = value;
        property.knowledge = StateKnowledge::Predicted;
        property.updatedAtMs = atMs;
        property.freshnessSteadyMs = steady;
        property.orderSteadyMs = order;
    }
}

void DeviceStateStore::validateObservationLocked(const StateObservation& observation,
                                                  const DeviceKnowledge& current,
                                                  std::int64_t arrival) const {
    const auto* device = domain_.device(observation.targetId);
    if (!device)
        throw config::ConfigError("unknown_target", "/observation/target_id",
            "Observation target missing from domain");
    const auto order = observation.orderSteadyMs.value_or(arrival);
    if ((observation.timestampMs != 0 && !observation.orderSteadyMs) || order > arrival)
        throw config::ConfigError("invalid_observation", "/observation",
            "Explicit event time requires a non-future process-local steady order stamp");
    for (const auto& [key, value] : observation.properties) {
        const auto found = current.properties.find(key);
        if (found == current.properties.end())
            throw config::ConfigError("unknown_field", "/observation/properties/" + key,
                "Observation property is not declared for the device");
        validateObservationValue(*device, key, value);
        if (found->second.orderSteadyMs && order < *found->second.orderSteadyMs)
            throw config::ConfigError("stale_observation", "/observation/properties/" + key,
                "Observation steady order is older than existing property knowledge");
    }
}
void DeviceStateStore::validateObservation(const StateObservation& observation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = devices_.find(observation.targetId);
    if (found == devices_.end())
        throw config::ConfigError("unknown_target", "/observation/target_id",
            "Observation target is not in the active generation");
    validateObservationLocked(observation, found->second, steadyNowMs());
}
void DeviceStateStore::applyObservation(const StateObservation& observation) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = devices_.find(observation.targetId);
    if (found == devices_.end())
        throw config::ConfigError("unknown_target", "/observation/target_id",
            "Observation target is not in the active generation");
    const auto arrival = steadyNowMs();
    validateObservationLocked(observation, found->second, arrival);
    if (observation.properties.empty())
        throw config::ConfigError("invalid_observation", "/observation/properties", "Empty observation is not device evidence");
    const auto order = observation.orderSteadyMs.value_or(arrival);
    const auto eventTime = observation.timestampMs == 0 ? nowMs() : observation.timestampMs;
    for (const auto& [key, value] : observation.properties) {
        auto& property = found->second.properties.at(key);
        property.value = value;
        property.knowledge = StateKnowledge::Confirmed;
        property.updatedAtMs = eventTime;
        property.observedAtMs = eventTime;
        property.freshnessSteadyMs = arrival;
        property.orderSteadyMs = order;
        property.lastObservedValue = value;
        property.lastObservationSteadyMs = arrival;
    }
    found->second.lastObservationAtMs = eventTime;
    found->second.lastObservationSteadyMs = arrival;
    found->second.observationDisconnected = false;
    found->second.connectivity = Connectivity::Reachable;
    found->second.lastSuccessAtMs = eventTime;
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
    found->second.lastObservationAtMs = 0;
    found->second.lastObservationSteadyMs.reset();
    found->second.observationDisconnected = false;
}

void DeviceStateStore::observationDisconnected(const std::string& targetId) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = devices_.find(targetId);
    if (found == devices_.end()) return;
    for (auto& [key, property] : found->second.properties) {
        (void)key;
        property.value.reset();
        property.knowledge = StateKnowledge::Unknown;
        property.updatedAtMs = 0;
        property.freshnessSteadyMs.reset();
        property.orderSteadyMs.reset();
        // observedAtMs / lastObservedValue / lastObservationSteadyMs survive.
    }
    found->second.observationDisconnected = true;
    found->second.connectivity = Connectivity::Unreachable;
    found->second.lastFailureAtMs = nowMs();
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
            if (property.lastObservedValue) {
                try { validateObservationValue(*domain_.device(old.targetId), key, *property.lastObservedValue); }
                catch (const config::ConfigError&) {
                    throw config::ConfigError("state_conflict", "/devices/" + old.targetId + "/state/" + key,
                        "Historical observation violates the proposed schema.");
                }
            }
            if (property.knowledge == StateKnowledge::Unknown || !property.value) {
                found->second.properties.at(key) = property;
                continue;
            }
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
        found->second.lastObservationAtMs = old.lastObservationAtMs;
        found->second.lastObservationSteadyMs = old.lastObservationSteadyMs;
        found->second.observationDisconnected = old.observationDisconnected;
    }
    devices_ = std::move(next);
}
}
