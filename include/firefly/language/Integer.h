#pragma once
#include <optional>
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>
namespace firefly::language {
struct IntegerToken { std::int64_t value; std::size_t count; };
// Exact digits or Russian cardinals 0..100; never fuzzy or transcript rewriting.
std::optional<IntegerToken> integerAt(const std::vector<std::string>& words, std::size_t offset);
bool percentWord(const std::string& word);
}
