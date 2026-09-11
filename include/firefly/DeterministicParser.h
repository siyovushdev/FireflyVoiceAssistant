#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace firefly {
class DeterministicParser {
public:
    // nullopt means unresolved, not authorization to execute anything.
    static std::optional<std::string> parse(std::string_view original);
};
}
