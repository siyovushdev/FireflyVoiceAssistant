#include "firefly/actions/GenericAction.h"
#include <utility>

namespace firefly::actions {
GenericAction::GenericAction(std::string actionId, std::string targetId, Arguments arguments)
    : actionId_(std::move(actionId)), targetId_(std::move(targetId)), arguments_(std::move(arguments)) {}

bool GenericAction::operator==(const GenericAction& other) const {
    return actionId_ == other.actionId_ && targetId_ == other.targetId_ && arguments_ == other.arguments_;
}
}
