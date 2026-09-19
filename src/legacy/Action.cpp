#include "firefly/legacy/Action.h"
#include <stdexcept>

namespace firefly::legacy {
std::string toCompact(const Action& action) {
    if (std::holds_alternative<OpenEntranceDoor>(action)) return "D";
    const auto light = std::get<LightAction>(action);
    if (light.brightness < 0 || light.brightness > 100)
        throw std::invalid_argument("brightness outside 0..100");
    char room;
    switch (light.room) {
        case Room::Kitchen: room = 'K'; break;
        case Room::Bedroom: room = 'B'; break;
        case Room::LivingRoom: room = 'G'; break;
        case Room::Corridor: room = 'C'; break;
        default: throw std::invalid_argument("unsupported room");
    }
    return std::string("L|") + room + "|" + std::to_string(light.brightness);
}
}
