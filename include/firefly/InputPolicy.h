#pragma once
#include <cstddef>
#include <istream>
#include <string>
#include <string_view>
namespace firefly {
inline constexpr std::size_t MaxInputBytes = 192;
// Empty/whitespace-only, invalid UTF-8, NUL/control bytes and oversized input fail.
bool validInput(std::string_view input);
// Drains overlong lines while retaining at most MaxInputBytes+1 bytes for rejection.
bool readInputLine(std::istream& stream, std::string& line);
}
