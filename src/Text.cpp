#include "Text.h"

namespace firefly::detail {
std::string lowercase(std::string_view text) {
    std::string result(text);
    // Explicit UTF-8 Russian/ASCII case folding, independent of process locale.
    for (std::size_t i = 0; i < result.size(); ++i) {
        auto first = static_cast<unsigned char>(result[i]);
        if (first >= 'A' && first <= 'Z') result[i] += 'a' - 'A';
        if (first != 0xD0 || i + 1 == result.size()) continue;
        auto second = static_cast<unsigned char>(result[i + 1]);
        if (second >= 0x90 && second <= 0x9F) result[i + 1] = static_cast<char>(second + 0x20);
        else if (second >= 0xA0 && second <= 0xAF) {
            result[i] = static_cast<char>(0xD1);
            result[i + 1] = static_cast<char>(second - 0x20);
        } else if (second == 0x81) {
            result[i] = static_cast<char>(0xD1);
            result[i + 1] = static_cast<char>(0x91);
        }
    }
    return result;
}
std::vector<std::string> words(std::string_view text) {
    const auto lower = lowercase(text);
    const std::string separators = " \t\r\n.,!?;:()[]{}\"'";
    std::vector<std::string> result;
    std::size_t start = lower.find_first_not_of(separators);
    while (start != std::string::npos) {
        const auto end = lower.find_first_of(separators, start);
        result.push_back(lower.substr(start, end - start));
        if (end == std::string::npos) break;
        start = lower.find_first_not_of(separators, end);
    }
    return result;
}
}
