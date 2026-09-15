#include "firefly/runtime/DomainView.h"
#include <utility>

namespace firefly::runtime {
DomainView::DomainView(config::Catalog snapshot) : snapshot_(std::move(snapshot)) {
    if (!snapshot_) throw std::invalid_argument("DomainView requires a validated snapshot");
}
const config::Room* DomainView::room(std::string_view id) const {
    const auto found = snapshot_->roomIndex().find(std::string(id));
    return found == snapshot_->roomIndex().end() ? nullptr :
        &snapshot_->configuration().rooms.at(found->second);
}
const config::Device* DomainView::device(std::string_view id) const {
    const auto found = snapshot_->deviceIndex().find(std::string(id));
    return found == snapshot_->deviceIndex().end() ? nullptr :
        &snapshot_->configuration().devices.at(found->second);
}
const config::Room* DomainView::roomMention(std::string_view alias) const {
    const auto found = snapshot_->mentionIndex().find(std::string(alias));
    return found == snapshot_->mentionIndex().end() ? nullptr : room(found->second);
}
const config::Room* DomainView::roomLocation(std::string_view alias) const {
    const auto found = snapshot_->locationIndex().find(std::string(alias));
    return found == snapshot_->locationIndex().end() ? nullptr : room(found->second);
}
const config::Device* DomainView::legacyLightTarget(char code) const {
    const auto& targets = snapshot_->configuration().compatibility.lightTargets;
    const auto found = targets.find(code);
    return found == targets.end() ? nullptr : device(found->second);
}
const config::Device* DomainView::entranceTarget() const {
    return device(snapshot_->configuration().compatibility.doorTargetId);
}
const config::EffectiveAction* DomainView::effectiveAction(std::string_view deviceId,
                                                        std::string_view actionId) const {
    for (const auto& action : snapshot_->effectiveActions())
        if (action.deviceId == deviceId && action.actionId == actionId) return &action;
    return nullptr;
}
std::optional<std::pair<std::int64_t, std::int64_t>> DomainView::brightnessBounds(std::string_view deviceId) const {
    const auto* target = device(deviceId);
    if (!target || target->kind != config::DeviceKind::Light) return std::nullopt;
    for (const auto& action : snapshot_->effectiveActions()) {
        if (action.deviceId == deviceId && action.operation == config::Operation::SetBrightness && action.permitted)
            return action.brightnessBounds;
    }
    return std::nullopt;
}
bool DomainView::allowsBrightness(std::string_view deviceId, std::int64_t value) const {
    const auto bounds = brightnessBounds(deviceId);
    return bounds && value >= bounds->first && value <= bounds->second;
}
bool DomainView::entranceAvailable() const {
    const auto* target = entranceTarget();
    if (!target || target->kind != config::DeviceKind::EntranceDoor || target->roomId) return false;
    const auto* action = effectiveAction(target->id, snapshot_->configuration().compatibility.doorActionId);
    return action && action->operation == config::Operation::OpenEntranceDoor && action->permitted;
}
}
