#pragma once
#include <string>
#include <string_view>
#include <vector>
namespace firefly::detail {
std::string lowercase(std::string_view text);
std::vector<std::string> words(std::string_view text);
}
