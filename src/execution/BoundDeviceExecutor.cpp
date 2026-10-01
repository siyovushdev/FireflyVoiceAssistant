#include "firefly/execution/ActionExecutor.h"
#include "firefly/execution/HttpHardwareExecutor.h"

namespace firefly::execution {
BoundDeviceExecutor::BoundDeviceExecutor(config::Catalog catalog, SteadyClockMs clock)
    : catalog_(catalog), playground_(catalog), production_(catalog, std::move(clock)), resolver_(catalog) {
    registry_.registerExecutor(config::ExecutorKind::Http, std::make_shared<HttpHardwareExecutor>());
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
}

DeviceStateStore& BoundDeviceExecutor::production() { return production_; }
const DeviceStateStore& BoundDeviceExecutor::production() const { return production_; }

void BoundDeviceExecutor::observe(const StateObservation& observation) {
    production_.applyObservation(observation);
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

    // Accepted ≠ Confirmed. Record Predicted knowledge only after transport accept.
    if (predicted.current) {
        std::map<std::string, actions::Scalar> expected;
        if (const auto* light = std::get_if<LightState>(&*predicted.current))
            expected.emplace("brightness", actions::Scalar(light->brightness));
        else if (const auto* door = std::get_if<DoorState>(&*predicted.current))
            expected.emplace("position", actions::Scalar(std::string(
                door->position == DoorPosition::Open ? "OPEN" : "CLOSED")));
        else if (const auto* typed = std::get_if<TypedState>(&*predicted.current)) {
            // Preview includes virtual initial/Playground values. Only assignments
            // actually made by this accepted action are production evidence.
            const auto& definition = catalog_->configuration().actions.at(catalog_->actionIndex().at(action.actionId()));
            for (const auto& assignment : definition.assignments) {
                if (assignment.argument && !action.arguments().count(*assignment.argument)) continue;
                expected.emplace(assignment.field, typed->at(assignment.field));
            }
        }
        production_.applyPrediction(action.targetId(), expected, now);
    }
    predicted.status = ExecutionStatus::Accepted;
    predicted.success = true;
    // Do not mutate playground simulation from production hardware acceptance.
    return predicted;
}
}
