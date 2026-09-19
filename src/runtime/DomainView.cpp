#include "firefly/runtime/DomainView.h"
#include <algorithm>
#include <utility>
namespace firefly::runtime {
DomainView::DomainView(config::Catalog snapshot) : snapshot_(std::move(snapshot)) {
    if (!snapshot_) throw std::invalid_argument("DomainView requires a validated snapshot");
}
const config::Room* DomainView::room(std::string_view id) const {
    const auto found = snapshot_->roomIndex().find(std::string(id));
    return found == snapshot_->roomIndex().end() ? nullptr : &snapshot_->configuration().rooms.at(found->second);
}
const config::Device* DomainView::device(std::string_view id) const {
    const auto found = snapshot_->deviceIndex().find(std::string(id));
    return found == snapshot_->deviceIndex().end() ? nullptr : &snapshot_->configuration().devices.at(found->second);
}
const config::Room* DomainView::roomMention(std::string_view alias) const {
    const auto found = snapshot_->mentionIndex().find(std::string(alias));
    return found == snapshot_->mentionIndex().end() ? nullptr : room(found->second);
}
const config::Room* DomainView::roomLocation(std::string_view alias) const {
    const auto found = snapshot_->locationIndex().find(std::string(alias));
    return found == snapshot_->locationIndex().end() ? nullptr : room(found->second);
}
std::vector<ParserRoomLocation> DomainView::parserLocations() const {
    std::vector<ParserRoomLocation> result;
    for (const auto& [phrase, roomId] : snapshot_->locationIndex()) {
        std::optional<ParserRoomLocation> selected;
        for (const auto& target : snapshot_->configuration().devices) {
            if (target.kind != config::DeviceKind::Light || target.roomId != roomId) continue;
            for (const auto& actionId : target.actionIds) {
                const auto& definition = snapshot_->configuration().actions.at(snapshot_->actionIndex().at(actionId));
                if (definition.operation != config::Operation::SetBrightness) continue;
                if (selected) throw std::invalid_argument("Ambiguous light target for location: " + phrase);
                selected = ParserRoomLocation{phrase, actionId, target.id, definition.parameters.at(0).id};
            }
        }
        if (!selected) throw std::invalid_argument("Missing light target for location: " + phrase);
        result.push_back(*selected);
    }
    return result;
}
const config::Device* DomainView::entranceTarget() const {
    return device(snapshot_->configuration().compatibility.doorTargetId);
}
const config::EffectiveAction* DomainView::effectiveAction(std::string_view deviceId, std::string_view actionId) const {
    for (const auto& action : snapshot_->effectiveActions())
        if (action.deviceId == deviceId && action.actionId == actionId) return &action;
    return nullptr;
}
std::optional<ActionDescription> DomainView::describe(const actions::GenericAction& candidate) const {
    const auto* target = device(candidate.targetId());
    const auto found = snapshot_->actionIndex().find(candidate.actionId());
    if (!target || found == snapshot_->actionIndex().end() ||
        std::find(target->actionIds.begin(), target->actionIds.end(), candidate.actionId()) == target->actionIds.end())
        return std::nullopt;
    const auto& definition = snapshot_->configuration().actions.at(found->second);
    if (candidate.arguments().size() != definition.parameters.size()) return std::nullopt;
    for (const auto& parameter : definition.parameters)
        if (!candidate.arguments().count(parameter.id)) return std::nullopt;
    if (definition.operation == config::Operation::SetBrightness && target->kind == config::DeviceKind::Light &&
        target->roomId && room(*target->roomId)) {
        return ActionDescription{target, &definition, candidate.arguments().at(definition.parameters.at(0).id).value()};
    }
    if (definition.operation == config::Operation::OpenEntranceDoor && target->kind == config::DeviceKind::EntranceDoor &&
        !target->roomId && candidate.arguments().empty()) return ActionDescription{target, &definition, std::nullopt};
    return std::nullopt;
}
actions::CandidateResult DomainView::validate(const actions::CandidateResult& candidate) const {
    const auto* action = std::get_if<actions::GenericAction>(&candidate);
    if (!action) return actions::Unsupported{};
    const auto description = describe(*action);
    if (!description) return actions::Unsupported{};
    if (description->brightness) {
        const auto* effective = effectiveAction(action->targetId(), action->actionId());
        if (!effective || !effective->brightnessBounds || *description->brightness < effective->brightnessBounds->first ||
            *description->brightness > effective->brightnessBounds->second) return actions::Unsupported{};
    } else {
        const auto* entrance = entranceTarget();
        if (!entrance || action->targetId() != entrance->id ||
            action->actionId() != snapshot_->configuration().compatibility.doorActionId) return actions::Unsupported{};
    }
    return candidate;
}
}
