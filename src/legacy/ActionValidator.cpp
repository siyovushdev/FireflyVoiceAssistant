#include "firefly/legacy/ActionValidator.h"

namespace firefly::legacy {
std::optional<Action> ActionValidator::validate(std::string_view candidate) {
    if (candidate == "D") return OpenEntranceDoor{};
    if (candidate.size() < 5 || candidate.substr(0, 2) != "L|" || candidate[3] != '|')
        return std::nullopt;
    Room room;
    switch (candidate[2]) {
        case 'K': room = Room::Kitchen; break;
        case 'B': room = Room::Bedroom; break;
        case 'G': room = Room::LivingRoom; break;
        case 'C': room = Room::Corridor; break;
        default: return std::nullopt;
    }
    int value = 0;
    for (char digit : candidate.substr(4)) {
        if (digit < '0' || digit > '9') return std::nullopt;
        value = value * 10 + digit - '0';
        if (value > 100) return std::nullopt;
    }
    return LightAction{room, value};
}
}
