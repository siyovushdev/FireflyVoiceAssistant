#pragma once
#include "firefly/Action.h"
#include <optional>
#include <string_view>

namespace firefly {
class ActionValidator {
public:
    // Exact protocol only; U and malformed candidates produce no action.
    static std::optional<Action> validate(std::string_view candidate);
};
}
