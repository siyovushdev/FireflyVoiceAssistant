#include "firefly/runtime/DomainView.h"
#include "firefly/config/LanguageProfile.h"
#include <algorithm>
#include <utility>
#include <sstream>
namespace firefly::runtime {
DomainView::DomainView(config::Catalog snapshot) : snapshot_(std::move(snapshot)) {
    if (!snapshot_) throw std::invalid_argument("DomainView requires a validated snapshot");
    if (snapshot_->configuration().schemaVersion == 2)
        for (const auto& room : snapshot_->configuration().rooms)
            if (!room.metadata.container) locationGrammar_.emplace(room.id, config::locationGrammar(room));
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
        bool ambiguous = false;
        for (const auto& target : snapshot_->configuration().devices) {
            if (target.kind != config::DeviceKind::Light || target.roomId != roomId) continue;
            for (const auto& actionId : target.actionIds) {
                const auto& definition = snapshot_->configuration().actions.at(snapshot_->actionIndex().at(actionId));
                if (definition.operation != config::Operation::SetBrightness) continue;
                if (selected) {
                    if (snapshot_->configuration().schemaVersion == 1) throw config::ConfigError("ambiguous_target", "/rooms/" + roomId, "Multiple brightness targets in location: " + room(roomId)->name);
                    ambiguous = true;continue;
                }
                selected = ParserRoomLocation{phrase, actionId, target.id, definition.parameters.at(0).id};
            }
        }
        if (!selected || ambiguous) continue; // A Location may contain no executable light device.
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
    for (const auto& parameter : definition.parameters) {
        const auto foundArgument = candidate.arguments().find(parameter.id);
        if (foundArgument == candidate.arguments().end()) { if (parameter.required) return std::nullopt; }
        else {
            // Range is checked by validate; structural types are checked here.
            const auto& data = foundArgument->second.data();
            if ((parameter.type == "integer" && !std::holds_alternative<std::int64_t>(data)) ||
                (parameter.type == "number" && !std::holds_alternative<double>(data) && !std::holds_alternative<std::int64_t>(data)) ||
                (parameter.type == "boolean" && !std::holds_alternative<bool>(data)) ||
                ((parameter.type == "string" || parameter.type == "enum") && !std::holds_alternative<std::string>(data))) return std::nullopt;
        }
    }
    for (const auto& [key, value] : candidate.arguments()) {
        (void)value;
        if (std::none_of(definition.parameters.begin(), definition.parameters.end(), [&](const auto& p) { return p.id == key; })) return std::nullopt;
    }
    if (definition.operation == config::Operation::AssignState && target->kind == config::DeviceKind::Generic)
        return ActionDescription{target, &definition, std::nullopt};
    if (definition.operation == config::Operation::SetBrightness && target->kind == config::DeviceKind::Light &&
        target->roomId && room(*target->roomId)) {
        return ActionDescription{target, &definition, candidate.arguments().at(definition.parameters.at(0).id).value()};
    }
    if (definition.operation == config::Operation::OpenEntranceDoor && target->kind == config::DeviceKind::EntranceDoor &&
        !target->roomId && candidate.arguments().empty()) return ActionDescription{target, &definition, std::nullopt};
    return std::nullopt;
}
std::string DomainView::validationReason(const actions::GenericAction& candidate) const {
    const auto description = describe(candidate);
    if (!description) return "Действие не поддерживается этим устройством или типы и состав аргументов неверны.";
    for (const auto& parameter : description->definition->parameters) {
        const auto argument = candidate.arguments().find(parameter.id);
        if (argument == candidate.arguments().end() || config::accepts(parameter, argument->second)) continue;
        std::ostringstream message;
        message << "Параметр «" << parameter.name << "»: ";
        if (parameter.type == "integer") message << "допустимо целое число " << parameter.minimum << ".." << parameter.maximum;
        else if (parameter.type == "number") message << "допустимо конечное число " << parameter.numberMinimum << ".." << parameter.numberMaximum;
        else if (parameter.type == "string") message << "допустим текст длиной " << parameter.minLength << ".." << parameter.maxLength;
        else if (parameter.type == "enum") { message << "выберите одно из: "; for (const auto& value : parameter.values) message << value << ' '; }
        else message << "требуется значение да/нет";
        return message.str();
    }
    return "Операция не разрешена для выбранного устройства.";
}
actions::CandidateResult DomainView::validate(const actions::CandidateResult& candidate) const {
    const auto* action = std::get_if<actions::GenericAction>(&candidate);
    if (!action) return actions::Unsupported{};
    const auto description = describe(*action);
    if (!description) return actions::Unsupported{};
    for (const auto& parameter : description->definition->parameters) {
        const auto argument = action->arguments().find(parameter.id);
        if (argument != action->arguments().end() && !config::accepts(parameter, argument->second)) return actions::Unsupported{};
    }
    if (description->definition->operation == config::Operation::AssignState) {
        const auto* effective = effectiveAction(action->targetId(), action->actionId());
        return effective && effective->permitted ? candidate : actions::CandidateResult(actions::Unsupported{});
    }
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
