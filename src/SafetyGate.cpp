#include "firefly/SafetyGate.h"
#include "firefly/ActionValidator.h"
#include "Text.h"
#include <algorithm>
#include <vector>

namespace firefly {
std::string SafetyGate::apply(std::string_view original, std::string_view candidate) {
    const auto action = ActionValidator::validate(candidate);
    if (!action) return "U";
    if (std::holds_alternative<OpenEntranceDoor>(*action)) {
        const auto text = detail::lowercase(original);
        const auto words = detail::words(original);
        const bool openIntent = std::any_of(words.begin(), words.end(), [](const auto& word) {
            return word == "открой" || word == "откройте" || word == "открыть" ||
                   word == "открывай" || word == "открывайте";
        });
        bool entranceReference = text.find("входн") != std::string::npos;
        for (std::size_t i = 1; i < words.size(); ++i) {
            if (words[i - 1] == "на" && words[i] == "входе") entranceReference = true;
        }
        // Require all three explicit cues in the original input. Never repair STT.
        if (!openIntent || !entranceReference ||
            text.find("двер") == std::string::npos) return "U";
        return std::string(candidate);
    }
    const auto light = std::get<LightAction>(*action);
    const auto words = detail::words(original);
    std::vector<std::string> rooms;
    switch (light.room) {
        case Room::Kitchen: rooms = {"кухня", "кухне", "кухню", "кухни", "кухней"}; break;
        case Room::Bedroom: rooms = {"спальня", "спальне", "спальню", "спальни", "спальней"}; break;
        case Room::LivingRoom: rooms = {"гостиная", "гостиной", "гостиную", "зал", "зале", "зала", "залу", "залом"}; break;
        case Room::Corridor: rooms = {"коридор", "коридоре", "коридора", "коридору", "коридором"}; break;
    }
    const bool mentioned = std::any_of(words.begin(), words.end(), [&](const auto& word) {
        return std::find(rooms.begin(), rooms.end(), word) != rooms.end();
    });
    if (!mentioned) return "U";
    // Conservative stem detection also covers inflections and mixed case.
    if (light.brightness == 0 && detail::lowercase(original).find("включ") != std::string::npos)
        return "U";
    return std::string(candidate);
}
}
