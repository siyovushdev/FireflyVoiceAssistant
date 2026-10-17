#include "firefly/execution/ActionExecutor.h"
#include "firefly/execution/HttpHardwareExecutor.h"
#include "firefly/execution/MqttHardwareExecutor.h"
#include <nlohmann/json.hpp>

namespace firefly::execution {
namespace {
// Transport-neutral identity of the physical mapping. Display-name edits retain
// evidence; changing endpoints/mappings invalidates it on generation publication.
std::map<std::string, std::string> physicalMappings(config::Catalog catalog) {
    using Json = nlohmann::json;
    const auto config = Json::parse(catalog->canonicalJson());
    std::map<std::string, Json> mappings;
    for (const auto* section : {"bindings", "observations"}) {
        for (const auto& item : config.value(section, Json::array())) {
            auto entry = item;
            if (item.contains("connection_id")) {
                for (auto connection : config.value("connections", Json::array())) {
                    if (connection.at("id") != item.at("connection_id")) continue;
                    connection.erase("name");
                    entry["connection"] = std::move(connection);
                }
            }
            mappings[item.at("target_id")][section].push_back(std::move(entry));
        }
    }
    std::map<std::string, std::string> result;
    for (const auto& [target, value] : mappings) result[target] = value.dump();
    return result;
}
}
BoundDeviceExecutor::BoundDeviceExecutor(config::Catalog catalog, SteadyClockMs steadyClock, WallClockMs wallClock)
    : catalog_(catalog), playground_(catalog), production_(catalog, std::move(steadyClock), std::move(wallClock)), resolver_(catalog) {
    registry_.registerExecutor(config::ExecutorKind::Http, std::make_shared<HttpHardwareExecutor>(catalog));
    mqtt_ = std::make_shared<MqttHardwareExecutor>(catalog, production_);
    registry_.registerExecutor(config::ExecutorKind::Mqtt, mqtt_);
}
void BoundDeviceExecutor::startSubscriptions() {
    if (mqtt_) mqtt_->startSubscriptions();
}

ExecutionResult BoundDeviceExecutor::preview(const actions::GenericAction& action) const {
    return playground_.preview(action);
}

std::vector<DeviceRecord> BoundDeviceExecutor::states() const { return playground_.states(); }

void BoundDeviceExecutor::reconcile(const std::vector<DeviceRecord>& previous) {
    playground_.reconcile(previous);
}

void BoundDeviceExecutor::reconcile(const BoundDeviceExecutor& previous) {
    playground_.reconcile(previous.playground_.states());
    production_.reconcile(previous.production_);
    auto before = physicalMappings(previous.catalog_), after = physicalMappings(catalog_);
    for (const auto& device : catalog_->configuration().devices)
        if (before[device.id] != after[device.id]) production_.invalidate(device.id);
}

DeviceStateStore& BoundDeviceExecutor::production() { return production_; }
const DeviceStateStore& BoundDeviceExecutor::production() const { return production_; }

void BoundDeviceExecutor::observe(const StateObservation& observation) {
    production_.applyObservation(observation);
}
HardwareResult BoundDeviceExecutor::poll(const std::string& targetId) {
    for (const auto& binding : catalog_->configuration().observations) {
        if (binding.targetId != targetId) continue;
        const auto order = production_.steadyNowMs();
        const auto observation = registry_.observe(binding);
        const auto now = production_.nowMs();
        if (observation.result.reachability == ReachabilityEvidence::Reachable) production_.markReachable(targetId, now);
        else if (observation.result.reachability == ReachabilityEvidence::Unreachable) production_.markUnreachable(targetId, now);
        if (!observation.result.success) return observation.result;
        try { production_.applyObservation({targetId, observation.properties, now, order}); }
        catch (const config::ConfigError&) { return {false, "invalid_observation", {}, observation.result.reachability}; }
        return observation.result;
    }
    return {false, "observe_missing", {}, ReachabilityEvidence::None};
}

ExecutionResult BoundDeviceExecutor::execute(const actions::GenericAction& action) {
    return execute(action, ExecutionIntent::Production);
}

ExecutionResult BoundDeviceExecutor::execute(const actions::GenericAction& action, ExecutionIntent intent) {
    const auto now = production_.nowMs();
    if (intent == ExecutionIntent::None)
        return {false, ExecutionStatus::Failed, action.actionId(), action.targetId(),
            "execution_intent_none", std::nullopt, std::nullopt, now};

    // Domain capability / argument validation only. SafetyGate already ran in
    // CommandEngine; neither playground nor production may bypass that reject.
    auto predicted = playground_.preview(action);
    predicted.timestampMs = now;
    predicted.status = predicted.success ? ExecutionStatus::Accepted : ExecutionStatus::Failed;
    if (!predicted.success) return predicted;

    if (intent == ExecutionIntent::Playground) {
        // Virtual simulation only — never touches DeviceStateStore / connectivity / HTTP.
        auto result = playground_.execute(action);
        result.timestampMs = now;
        result.status = result.success ? ExecutionStatus::Accepted : ExecutionStatus::Failed;
        return result;
    }

    // Production: fail closed without a binding. Never fall back to playground success.
    // binding_missing / ambiguous → no connectivity evidence (no transport attempt).
    const auto lookup = resolver_.resolve(action.targetId(), action.actionId());
    if (lookup.status == BindingResolver::Status::Missing) {
        predicted.success = false;
        predicted.status = ExecutionStatus::Failed;
        predicted.error = "binding_missing";
        predicted.current = std::nullopt;
        return predicted;
    }
    if (lookup.status == BindingResolver::Status::Ambiguous) {
        predicted.success = false;
        predicted.status = ExecutionStatus::Failed;
        predicted.error = "binding_ambiguous";
        predicted.current = std::nullopt;
        return predicted;
    }

    // Derive state assignments once, before any physical action. The MQTT
    // barrier can only buffer a matching early state, never an unrelated OFF.
    std::map<std::string, actions::Scalar> expected;
    if (predicted.current) {
        if (const auto* light = std::get_if<LightState>(&*predicted.current))
            expected.emplace("brightness", actions::Scalar(light->brightness));
        else if (const auto* door = std::get_if<DoorState>(&*predicted.current))
            expected.emplace("position", actions::Scalar(std::string(
                door->position == DoorPosition::Open ? "OPEN" : "CLOSED")));
        else if (const auto* typed = std::get_if<TypedState>(&*predicted.current)) {
            const auto& def = catalog_->configuration().actions.at(catalog_->actionIndex().at(action.actionId()));
            for (const auto& assign : def.assignments) {
                if (assign.argument && !action.arguments().count(*assign.argument)) continue;
                expected.emplace(assign.field, typed->at(assign.field));
            }
        }
    }
    struct MqttCommandWindow {
        MqttHardwareExecutor* owner = nullptr;
        std::string target;
        bool committed = false;
        ~MqttCommandWindow() { if (owner) owner->endCommand(target, committed); }
    } window;
    if (lookup.binding->executor == config::ExecutorKind::Mqtt && mqtt_) {
        window.target = action.targetId();
        try {
            mqtt_->beginCommand(window.target, expected);
        } catch (const std::logic_error&) {
            // The per-target barrier rejects concurrent MQTT commands. Report
            // a normal failed execution rather than letting a second request
            // unwind through the Admin/voice runtime.
            predicted.success = false;
            predicted.status = ExecutionStatus::Failed;
            predicted.error = "mqtt_target_busy";
            predicted.current = std::nullopt;
            return predicted;
        }
        window.owner = mqtt_.get();
    }
    const auto hardware = registry_.execute(*lookup.binding, action);
    // Connectivity follows transport evidence, independent of Accepted|Failed.
    if (hardware.reachability == ReachabilityEvidence::Reachable)
        production_.markReachable(action.targetId(), now);
    else if (hardware.reachability == ReachabilityEvidence::Unreachable)
        production_.markUnreachable(action.targetId(), now);

    if (!hardware.success) {
        predicted.success = false;
        predicted.status = ExecutionStatus::Failed;
        predicted.error = hardware.error;
        if (!hardware.detail.empty()) predicted.error += ":" + hardware.detail;
        predicted.current = std::nullopt;
        return predicted;
    }

    // Only transport acceptance creates Predicted; after this, a validated
    // pre-PUBACK observation may be safely replayed as Confirmed by the guard.
    if (predicted.current) production_.applyPrediction(action.targetId(), expected, now);
    window.committed = true;
    predicted.status = ExecutionStatus::Accepted;
    predicted.success = true;
    // Do not mutate playground simulation from production hardware acceptance.
    return predicted;
}
}
