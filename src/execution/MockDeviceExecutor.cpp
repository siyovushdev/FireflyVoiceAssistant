#include "firefly/execution/ActionExecutor.h"

namespace firefly::execution {
MockDeviceExecutor::MockDeviceExecutor(config::Catalog catalog) : domain_(std::move(catalog)) {
    for (const auto& device : domain_.snapshot()->configuration().devices) {
        if (!device.enabled) continue;
        DeviceState initial;
        if (device.kind == config::DeviceKind::Light) {
            std::optional<std::int64_t> minimum;
            for (const auto& id : device.actionIds) {
                const auto* effective = domain_.effectiveAction(device.id, id);
                if (effective && effective->permitted && effective->brightnessBounds) {
                    if (minimum) throw std::invalid_argument("Ambiguous mock light capability");
                    minimum = effective->brightnessBounds->first;
                }
            }
            if (!minimum) throw std::invalid_argument("Missing mock light capability");
            initial = LightState{*minimum};
        } else if (device.kind == config::DeviceKind::EntranceDoor) initial = DoorState{};
        else throw std::invalid_argument("Unsupported mock device kind");
        devices_.emplace(device.id, DeviceRecord{device.id, device.name, initial});
    }
}
ExecutionResult MockDeviceExecutor::execute(const actions::GenericAction& action) {
    std::lock_guard<std::mutex> lock(mutex_);
    ExecutionResult result;
    result.actionId = action.actionId(); result.targetId = action.targetId();
    const auto found = devices_.find(action.targetId());
    const auto* effective = domain_.effectiveAction(action.targetId(), action.actionId());
    const auto description = domain_.describe(action);
    if (found == devices_.end() || !effective || !effective->permitted || !description ||
        !std::holds_alternative<actions::GenericAction>(domain_.validate(action))) {
        result.error = "invalid_action_domain"; return result;
    }
    DeviceState next;
    if (effective->operation == config::Operation::SetBrightness && description->brightness &&
        std::holds_alternative<LightState>(found->second.state)) next = LightState{*description->brightness};
    else if (effective->operation == config::Operation::OpenEntranceDoor &&
             std::holds_alternative<DoorState>(found->second.state)) next = DoorState{DoorPosition::Open};
    else { result.error = "unsupported_operation"; return result; }
    result.previous = found->second.state; result.current = next;
    found->second.state = next; result.success = true;
    return result;
}
std::vector<DeviceRecord> MockDeviceExecutor::states() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DeviceRecord> result;
    for (const auto& entry : devices_) result.push_back(entry.second);
    return result;
}
void MockDeviceExecutor::reconcile(const std::vector<DeviceRecord>& previous) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto next = devices_;
    for (const auto& old : previous) {
        auto found = next.find(old.targetId);
        if (found == next.end()) continue;
        if (found->second.state.index() != old.state.index())
            throw config::ConfigError("state_conflict", "/devices", "Retained device changed state type.");
        if (const auto* light = std::get_if<LightState>(&old.state)) {
            const auto* device = domain_.device(old.targetId);
            for (const auto& id : device->actionIds) {
                const auto* effective = domain_.effectiveAction(old.targetId, id);
                if (effective && effective->brightnessBounds &&
                    (light->brightness < effective->brightnessBounds->first || light->brightness > effective->brightnessBounds->second))
                    throw config::ConfigError("state_conflict", "/devices/" + old.targetId,
                        "Current mock brightness is outside the proposed range. Execute a value valid in both ranges before Apply.");
            }
        }
        found->second.state = old.state;
    }
    devices_ = std::move(next);
}
}
