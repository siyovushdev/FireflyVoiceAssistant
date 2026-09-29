#include "firefly/language/Integer.h"
#include <charconv>

namespace firefly::language {
namespace {
int cardinal(const std::string& word) {
    static const char* small[] = {
        "ноль", "один", "два", "три", "четыре", "пять", "шесть", "семь", "восемь", "девять",
        "десять", "одиннадцать", "двенадцать", "тринадцать", "четырнадцать", "пятнадцать",
        "шестнадцать", "семнадцать", "восемнадцать", "девятнадцать"};
    for (int i = 0; i < 20; ++i) if (word == small[i]) return i;
    static const char* tens[] = {
        "двадцать", "тридцать", "сорок", "пятьдесят", "шестьдесят", "семьдесят", "восемьдесят", "девяносто", "сто"};
    for (int i = 0; i < 9; ++i) if (word == tens[i]) return (i + 2) * 10;
    return word == "нуль" ? 0 : -1;
}
}

bool percentWord(const std::string& word) {
    return word == "%" || word == "процент" || word == "процента" || word == "процентов";
}

std::optional<IntegerToken> integerAt(const std::vector<std::string>& words, std::size_t offset) {
    if (offset >= words.size()) return {};
    auto token = words[offset];
    if (!token.empty() && token.back() == '%') token.pop_back();
    std::int64_t value = 0;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (!token.empty() && parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size())
        return IntegerToken{value, 1};
    const int number = cardinal(token);
    if (number < 0) return {};
    std::size_t count = 1;
    value = number;
    if (number >= 20 && number <= 90 && offset + 1 < words.size()) {
        const int unit = cardinal(words[offset + 1]);
        if (unit >= 1 && unit <= 9) { value += unit; count = 2; }
    }
    return IntegerToken{value, count};
}
}
