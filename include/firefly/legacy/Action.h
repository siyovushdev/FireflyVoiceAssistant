#pragma once
#include <string>
#include <variant>

namespace firefly::legacy {
enum class Room { Kitchen, Bedroom, LivingRoom, Corridor };
struct LightAction { Room room; int brightness; };
struct OpenEntranceDoor {};
using Action = std::variant<LightAction, OpenEntranceDoor>;
// Rejects invalid manually constructed values with std::invalid_argument.
std::string toCompact(const Action& action);
}
