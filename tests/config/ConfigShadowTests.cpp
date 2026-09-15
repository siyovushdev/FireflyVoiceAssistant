#include "firefly/config/ConfigLoader.h"
#include "firefly/Action.h"
#include "firefly/ActionValidator.h"
#include "firefly/DeterministicParser.h"
#include "firefly/SafetyGate.h"
#include <algorithm>
#include <iostream>

// This executable alone links both domains; neither production nor the config
// library links the other. Expected identities are independent test fixtures.
int main(int argc, char** argv) {
    int checks = 0;
    const auto check = [&](bool result, const std::string& message) {
        ++checks; if (!result) throw std::runtime_error(message);
    };
    try {
        if (argc != 2) throw std::runtime_error("Repository root required");
        const auto catalog = firefly::config::ConfigLoader::fromFile(std::string(argv[1]) + "/config/current-m4.json");
        const auto& config = catalog->configuration();
        struct Expected { firefly::Room room; char code; std::string id, device; };
        const std::vector<Expected> expected{
            {firefly::Room::Kitchen, 'K', "room_kitchen", "device_kitchen_light"},
            {firefly::Room::Bedroom, 'B', "room_bedroom", "device_bedroom_light"},
            {firefly::Room::LivingRoom, 'G', "room_living", "device_living_light"},
            {firefly::Room::Corridor, 'C', "room_corridor", "device_corridor_light"}};
        check(config.rooms.size() == expected.size() && config.devices.size() == 5 && config.actions.size() == 2, "same current entity counts");
        for (const auto& entry : expected) {
            const auto& room = config.rooms.at(catalog->roomIndex().at(entry.id));
            const auto& device = config.devices.at(catalog->deviceIndex().at(entry.device));
            check(device.roomId == entry.id && config.compatibility.lightTargets.at(entry.code) == entry.device, "same code/room/device mapping");
            const auto& action = config.actions.at(catalog->actionIndex().at(device.actionIds.at(0)));
            check(action.parameters.size() == 1 && action.parameters[0].minimum == 0 && action.parameters[0].maximum == 100, "same absolute integer domain");
            for (int brightness = 0; brightness <= 100; ++brightness) {
                const std::string compact = "L|" + std::string(1, entry.code) + "|" + std::to_string(brightness);
                check(firefly::toCompact(firefly::LightAction{entry.room, brightness}) == compact, "old enum codec unchanged");
                const auto parsed = firefly::ActionValidator::validate(compact);
                check(parsed && firefly::toCompact(*parsed) == compact, "old strict validator accepts same range");
                for (const auto& location : room.locations)
                    check(firefly::DeterministicParser::parse(location + " свет " + std::to_string(brightness)) == compact, "configured locations equal old fast path");
            }
            for (const auto& mention : room.mentions) {
                const std::string candidate = "L|" + std::string(1, entry.code) + "|40";
                check(firefly::SafetyGate::apply(mention + " свет 40", candidate) == candidate, "configured mention is explicit old gate evidence");
                check(firefly::SafetyGate::apply(mention + " свет 40", "L|" + std::string(1, entry.code == 'K' ? 'B' : 'K') + "|40") == "U", "mention cannot authorize other room");
            }
            check(!firefly::ActionValidator::validate("L|" + std::string(1, entry.code) + "|101"), "101 unsupported");
            check(!firefly::ActionValidator::validate("L|" + std::string(1, entry.code) + "|-1"), "negative unsupported");
        }
        const auto& door = config.devices.at(catalog->deviceIndex().at(config.compatibility.doorTargetId));
        check(door.id == "device_entrance_door" && door.kind == firefly::config::DeviceKind::EntranceDoor && !door.roomId, "same one roomless entrance target");
        const auto& opening = config.actions.at(catalog->actionIndex().at(config.compatibility.doorActionId));
        check(opening.operation == firefly::config::Operation::OpenEntranceDoor && opening.parameters.empty(), "same door action and no invented parameters");
        check(firefly::toCompact(firefly::OpenEntranceDoor{}) == "D", "legacy D unchanged");
        check(!firefly::DeterministicParser::parse("открой входную дверь"), "door route remains fallback");
        for (const auto* verb : {"открой", "откройте", "открыть", "открывай", "открывайте"})
            check(firefly::SafetyGate::apply(std::string(verb) + " входную дверь", "D") == "D", "same explicit door forms");
        check(firefly::SafetyGate::apply("Открыть дверь на входе", "D") == "D", "frozen DEV entrance form preserved");
        check(firefly::SafetyGate::apply("открытвер на входе", "D") == "U", "recorded M4 false reject remains uncorrected");
        check(firefly::SafetyGate::apply("открой дверь", "D") == "U", "missing entrance fails closed");
        check(!firefly::ActionValidator::validate("U"), "U is not an action");
        std::cout << "ConfigShadow: " << checks << " checks passed; 404 light values and one entrance target agree\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Shadow failure after " << checks << " checks: " << error.what() << '\n'; return 1;
    }
}
