#pragma once
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace firefly::admin {
inline constexpr std::size_t MaxMessageCodePoints = 4000;
inline constexpr std::size_t MaxAdminMessageBytes = 16000;
// Application message admission, independent of model context admission.
inline std::size_t validateMessage(std::string_view text) {
    if (text.empty() || text.size() > MaxAdminMessageBytes)
        throw std::invalid_argument("admin_input_size: максимум 4000 символов / 16000 UTF-8 байт");
    std::size_t count = 0;
    bool nonblank = false;
    for (std::size_t i = 0; i < text.size();) {
        auto c = static_cast<unsigned char>(text[i++]);
        std::uint32_t cp = c, minimum = 0;
        int continuation = 0;
        if (c < 128) {}
        else if (c >= 0xc2 && c <= 0xdf) { cp = c & 31; minimum = 128; continuation = 1; }
        else if (c >= 0xe0 && c <= 0xef) { cp = c & 15; minimum = 2048; continuation = 2; }
        else if (c >= 0xf0 && c <= 0xf4) { cp = c & 7; minimum = 65536; continuation = 3; }
        else throw std::invalid_argument("admin_invalid_utf8");
        while (continuation--) {
            if (i == text.size()) throw std::invalid_argument("admin_invalid_utf8");
            auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 192) != 128) throw std::invalid_argument("admin_invalid_utf8");
            cp = (cp << 6) | (next & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            throw std::invalid_argument("admin_invalid_utf8");
        if ((cp < 32 && cp != 9 && cp != 10 && cp != 13) || (cp >= 127 && cp <= 159) ||
            (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2066 && cp <= 0x2069))
            throw std::invalid_argument("admin_control_character");
        if (cp != 32 && cp != 9 && cp != 10 && cp != 13 && cp != 160 && cp != 0x200b && cp != 0xfeff && !(cp >= 0x2000 && cp <= 0x200a) && cp != 0x2028 && cp != 0x2029 && cp != 0x202f && cp != 0x205f && cp != 0x3000)
            nonblank = true;
        if (++count > MaxMessageCodePoints) throw std::invalid_argument("admin_input_size: максимум 4000 символов");
    }
    if (!nonblank) throw std::invalid_argument("admin_empty_message");
    return count;
}
}
