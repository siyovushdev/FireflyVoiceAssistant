#include "firefly/execution/ActionExecutor.h"
#include "firefly/execution/HttpHardwareExecutor.h"

namespace firefly::execution {
BoundDeviceExecutor::BoundDeviceExecutor(config::Catalog catalog)
    : mock_(catalog), resolver_(catalog) {
    registry_.registerExecutor(config::ExecutorKind::Http, std::make_shared<HttpHardwareExecutor>());
}

ExecutionResult BoundDeviceExecutor::preview(const actions::GenericAction& action) const {
    return mock_.preview(action);
}

std::vector<DeviceRecord> BoundDeviceExecutor::states() const { return mock_.states(); }

void BoundDeviceExecutor::reconcile(const std::vector<DeviceRecord>& previous) {
    mock_.reconcile(previous);
}

ExecutionResult BoundDeviceExecutor::execute(const actions::GenericAction& action) {
    return execute(action, ExecutionIntent::Production);
}

ExecutionResult BoundDeviceExecutor::execute(const actions::GenericAction& action, ExecutionIntent intent) {
    if (intent == ExecutionIntent::None)
        return {false, action.actionId(), action.targetId(), "execution_intent_none", std::nullopt, std::nullopt};

    // Domain capability / argument validation only. SafetyGate already ran in
    // CommandEngine; neither playground nor production may bypass that reject.
    auto predicted = mock_.preview(action);
    if (!predicted.success) return predicted;

    if (intent == ExecutionIntent::Playground)
        return mock_.execute(action);

    // Production: fail closed without a binding. Never fall back to mock success.
    const auto lookup = resolver_.resolve(action.targetId(), action.actionId());
    if (lookup.status == BindingResolver::Status::Missing) {
        predicted.success = false;
        predicted.error = "binding_missing";
        predicted.current = std::nullopt;
        return predicted;
    }
    if (lookup.status == BindingResolver::Status::Ambiguous) {
        predicted.success = false;
        predicted.error = "binding_ambiguous";
        predicted.current = std::nullopt;
        return predicted;
    }

    const auto hardware = registry_.execute(*lookup.binding, action);
    if (!hardware.success) {
        predicted.success = false;
        predicted.error = hardware.error;
        if (!hardware.detail.empty()) predicted.error += ":" + hardware.detail;
        predicted.current = std::nullopt;
        return predicted;
    }
    // Command accepted by the transport. Local mock state follows the existing
    // deterministic action model; this is not independent physical telemetry.
    return mock_.execute(action);
}
}
