#include "firefly/actions/CompactV1Adapter.h"
#include "firefly/Action.h"
#include "firefly/ActionValidator.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace firefly::actions {
CompactV1Adapter::CompactV1Adapter(config::Catalog snapshot) : snapshot_(std::move(snapshot)) {
    if (!snapshot_) throw std::invalid_argument("CompactV1Adapter requires a validated snapshot");
}

CompactDecodeResult CompactV1Adapter::decode(std::string_view compact) const {
    if (compact == "U") return CandidateResult{Unsupported{}};
    // Preserve the exact frozen syntax, including leading-zero acceptance. No
    // second room-code/range table is introduced by this adapter.
    const auto legacy = ActionValidator::validate(compact);
    if (!legacy) return CompactV1Error::InvalidSyntax;
    const auto& config = snapshot_->configuration();
    const auto& mapping = config.compatibility;
    if (std::holds_alternative<OpenEntranceDoor>(*legacy)) {
        return CandidateResult{GenericAction(mapping.doorActionId, mapping.doorTargetId, {})};
    }
    const auto canonical = toCompact(*legacy);
    const auto target = mapping.lightTargets.find(canonical[2]);
    if (target == mapping.lightTargets.end()) return CompactV1Error::Unrepresentable;
    const auto& definition = config.actions.at(snapshot_->actionIndex().at(mapping.lightActionId));
    return CandidateResult{GenericAction(mapping.lightActionId, target->second,
        {{definition.parameters.at(0).id,
          IntegerArgument(std::int64_t{std::get<LightAction>(*legacy).brightness})}})};
}

CompactEncodeResult CompactV1Adapter::encode(const GenericAction& action) const {
    const auto actionIndex = snapshot_->actionIndex().find(action.actionId());
    if (actionIndex == snapshot_->actionIndex().end()) return CompactV1Error::UnknownAction;
    const auto targetIndex = snapshot_->deviceIndex().find(action.targetId());
    if (targetIndex == snapshot_->deviceIndex().end()) return CompactV1Error::UnknownTarget;

    const auto& config = snapshot_->configuration();
    const auto& definition = config.actions.at(actionIndex->second);
    const auto& target = config.devices.at(targetIndex->second);
    if (std::find(target.actionIds.begin(), target.actionIds.end(), action.actionId()) == target.actionIds.end())
        return CompactV1Error::Unrepresentable;
    if (action.arguments().size() != definition.parameters.size()) return CompactV1Error::InvalidArguments;
    for (const auto& parameter : definition.parameters) {
        if (action.arguments().find(parameter.id) == action.arguments().end())
            return CompactV1Error::InvalidArguments;
    }

    const auto& mapping = config.compatibility;
    if (definition.operation == config::Operation::OpenEntranceDoor) {
        if (target.kind != config::DeviceKind::EntranceDoor || target.roomId ||
            action.actionId() != mapping.doorActionId || action.targetId() != mapping.doorTargetId)
            return CompactV1Error::Unrepresentable;
        return toCompact(OpenEntranceDoor{});
    }
    if (definition.operation != config::Operation::SetBrightness || target.kind != config::DeviceKind::Light ||
        action.actionId() != mapping.lightActionId)
        return CompactV1Error::Unrepresentable;
    std::optional<char> code;
    for (const auto& [candidate, targetId] : mapping.lightTargets) {
        if (targetId == action.targetId()) {
            if (code) return CompactV1Error::Unrepresentable;
            code = candidate;
        }
    }
    if (!code) return CompactV1Error::Unrepresentable;
    const auto value = action.arguments().at(definition.parameters.at(0).id).value();
    // int64 -> decimal -> frozen decoder checks the protocol envelope without
    // truncation, narrowing, a duplicated 0..100 check or a configured-range check.
    const auto legacy = ActionValidator::validate(std::string("L|") + *code + "|" + std::to_string(value));
    if (!legacy) return CompactV1Error::InvalidArguments;
    return toCompact(*legacy);
}
}
