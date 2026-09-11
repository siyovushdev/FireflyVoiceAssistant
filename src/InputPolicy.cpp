#include "firefly/InputPolicy.h"
#include <cstdint>
namespace firefly {
bool validInput(std::string_view input) {
    if (input.empty() || input.size() > MaxInputBytes ||
        input.find_first_not_of(" \t\r\n\f\v") == std::string_view::npos) return false;
    for (std::size_t i = 0; i < input.size();) {
        const auto c = static_cast<unsigned char>(input[i++]);
        if (c < 0x80) {
            if (c == 0x7f || (c < 0x20 && std::string_view(" \t\r\n\f\v").find(c) == std::string_view::npos))
                return false;
            continue;
        }
        int remaining;
        std::uint32_t code;
        std::uint32_t minimum;
        if (c >= 0xc2 && c <= 0xdf) { remaining = 1; code = c & 0x1f; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { remaining = 2; code = c & 0x0f; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { remaining = 3; code = c & 7; minimum = 0x10000; }
        else return false;
        while (remaining--) {
            if (i == input.size()) return false;
            const auto next = static_cast<unsigned char>(input[i++]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}
bool readInputLine(std::istream& stream, std::string& line) {
    line.clear();
    char c;
    bool read = false;
    while (stream.get(c)) {
        read = true;
        if (c == '\n') break;
        if (line.size() <= MaxInputBytes) line.push_back(c);
    }
    return read;
}
}
