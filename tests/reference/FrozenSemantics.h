#pragma once
#include <optional>
#include <string>
#include <string_view>
namespace firefly::reference {
// Frozen pre-M7 reference only. Never linked by a production target.
struct FrozenParser { static std::optional<std::string> parse(std::string_view original); };
struct FrozenSafetyGate { static std::string apply(std::string_view original, std::string_view candidate); };
}
