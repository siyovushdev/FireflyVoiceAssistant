#pragma once
#include <nlohmann/json.hpp>
#include "firefly/actions/ScalarJson.h"
#include <map>
#include <set>
#include <stdexcept>
namespace firefly::admin {
using Json = nlohmann::json;
inline constexpr std::size_t MaxMessageBytes = 512 * 1024;
inline Json parseMessage(std::string_view bytes, bool typedNumbers = false) {
    if (bytes.empty() || bytes.size() > MaxMessageBytes || bytes.substr(0, 3) == "\xef\xbb\xbf")
        throw std::invalid_argument("Invalid JSON message");
    if (typedNumbers) actions::checkNumericLexemes(bytes);
    std::map<int, std::set<std::string>> keys;
    auto result = Json::parse(bytes.begin(), bytes.end(), [&](int depth, Json::parse_event_t event, Json& item) {
        using Event = Json::parse_event_t;
        if (depth > 24) throw std::invalid_argument("Message too deep");
        if (event == Event::object_start) keys[depth] = {};
        if (event == Event::object_end) keys.erase(depth);
        if (event == Event::key && !keys.at(depth - 1).insert(item.get<std::string>()).second)
            throw std::invalid_argument("Duplicate JSON member");
        return true;
    });
    if (!result.is_object()) throw std::invalid_argument("Object required");
    return result;
}
inline void members(const Json& value, const std::set<std::string>& fields) {
    if (!value.is_object() || value.size() != fields.size()) throw std::invalid_argument("Unexpected fields");
    for (const auto& field : fields) if (!value.contains(field)) throw std::invalid_argument("Missing field");
}
inline Json failure(std::string code, std::string path, std::string message) {
    return {{"ok", false}, {"errors", Json::array({{{"code", code}, {"path", path}, {"message", message}}})}};
}
}
