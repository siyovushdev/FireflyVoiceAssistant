#include "firefly/execution/ActionExecutor.h"
#include <algorithm>

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
        else if (device.kind == config::DeviceKind::Generic) {
            if (device.state.empty()) initial = InventoryState{};
            else {
                TypedState values;
                for (const auto& field : device.state) values.emplace(field.id, field.initial);
                initial = std::move(values);
            }
        }
        else throw std::invalid_argument("Unsupported mock device kind");
        devices_.emplace(device.id, DeviceRecord{device.id, device.name, initial});
    }
}
ExecutionResult MockDeviceExecutor::execute(const actions::GenericAction& action) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = evaluate(action);
    if (result.success) devices_.at(action.targetId()).state = *result.current;
    return result;
}
ExecutionResult MockDeviceExecutor::preview(const actions::GenericAction& action) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return evaluate(action);
}
ExecutionResult MockDeviceExecutor::evaluate(const actions::GenericAction& action) const {
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
    else if (effective->operation == config::Operation::AssignState && std::holds_alternative<TypedState>(found->second.state)) {
        auto values = std::get<TypedState>(found->second.state);
        for (const auto& assignment : description->definition->assignments) {
            const actions::Scalar* value = assignment.value ? &*assignment.value : nullptr;
            if (assignment.argument) {
                const auto argument = action.arguments().find(*assignment.argument);
                if (argument == action.arguments().end()) continue; // Optional argument: leave its field unchanged.
                value = &argument->second;
            }
            const auto field = std::find_if(description->target->state.begin(), description->target->state.end(),
                [&](const auto& field) { return field.id == assignment.field; });
            if (!value || field == description->target->state.end() || !values.count(assignment.field) || !config::accepts(*field, *value)) {
                result.error = "invalid_state_assignment"; return result;
            }
            values.at(assignment.field) = field->type == "number" && std::holds_alternative<std::int64_t>(value->data())
                ? actions::Scalar(static_cast<double>(value->value())) : *value;
        }
        next = std::move(values);
    }
    else { result.error = "unsupported_operation"; return result; }
    result.previous = found->second.state; result.current = next;
    result.success = true;
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
        // Inventory has no user state to discard. Activating its first declared
        // state is an explicit initialization, shown by the Apply state preview.
        if (std::holds_alternative<InventoryState>(old.state) && std::holds_alternative<TypedState>(found->second.state)) continue;
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
        if (const auto* values = std::get_if<TypedState>(&old.state)) {
            const auto* device = domain_.device(old.targetId);
            if (values->size() != device->state.size())
                throw config::ConfigError("state_conflict", "/devices/" + old.targetId, "Retained state fields changed; Apply does not reset or discard state implicitly.");
            for (const auto& field : device->state) {
                const auto value = values->find(field.id);
                if (value == values->end() || !config::accepts(field, value->second))
                    throw config::ConfigError("state_conflict", "/devices/" + old.targetId + "/state/" + field.id, "Retained state violates the proposed schema.");
            }
        }
        found->second.state = old.state;
    }
    devices_ = std::move(next);
}
}
