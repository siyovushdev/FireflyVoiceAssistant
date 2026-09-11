#pragma once
#include <string>
#include <string_view>

namespace firefly {
class SafetyGate {
public:
    // Always supply the original user input. Output equals candidate or U.
    static std::string apply(std::string_view original, std::string_view candidate);
};
}
