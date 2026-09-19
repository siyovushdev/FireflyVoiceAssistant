#include "TestSupport.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/legacy/Action.h"
#include "firefly/legacy/ActionValidator.h"
#include "firefly/DeterministicParser.h"
#include "firefly/SafetyGate.h"
#include <algorithm>
#include <iostream>

// Offline compatibility checks. Expected identities are independent fixtures;
// semantic assertions call the real typed production components.
int main(int argc, char** argv) {
    int checks = 0;
    const auto check = [&](bool result, const std::string& message) {
        ++checks; if (!result) throw std::runtime_error(message);
    };
    try {
        if (argc != 2) throw std::runtime_error("Repository root required");
        const auto catalog = firefly::config::ConfigLoader::fromFile(std::string(argv[1]) + "/config/current-m4.json");
        const auto& config = catalog->configuration();
        struct Expected { firefly::legacy::Room room; char code; std::string id, device; };
        const std::vector<Expected> expected{
            {firefly::legacy::Room::Kitchen, 'K', "room_kitchen", "device_kitchen_light"},
            {firefly::legacy::Room::Bedroom, 'B', "room_bedroom", "device_bedroom_light"},
            {firefly::legacy::Room::LivingRoom, 'G', "room_living", "device_living_light"},
            {firefly::legacy::Room::Corridor, 'C', "room_corridor", "device_corridor_light"}};
        check(config.rooms.size() == expected.size() && config.devices.size() == 5 && config.actions.size() == 2, "same current entity counts");
        for (const auto& entry : expected) {
            const auto& room = config.rooms.at(catalog->roomIndex().at(entry.id));
            const auto& device = config.devices.at(catalog->deviceIndex().at(entry.device));
            check(device.roomId == entry.id && config.compatibility.lightTargets.at(entry.code) == entry.device, "same code/room/device mapping");
            const auto& action = config.actions.at(catalog->actionIndex().at(device.actionIds.at(0)));
            check(action.parameters.size() == 1 && action.parameters[0].minimum == 0 && action.parameters[0].maximum == 100, "same absolute integer domain");
            for (int brightness = 0; brightness <= 100; ++brightness) {
                const std::string compact = "L|" + std::string(1, entry.code) + "|" + std::to_string(brightness);
                check(firefly::legacy::toCompact(firefly::legacy::LightAction{entry.room, brightness}) == compact, "old enum codec unchanged");
                const auto parsed = firefly::legacy::ActionValidator::validate(compact);
                check(parsed && firefly::legacy::toCompact(*parsed) == compact, "old strict validator accepts same range");
                for (const auto& location : room.locations)
                    check(test::parse(location + " свет " + std::to_string(brightness)) == compact, "configured locations equal old fast path");
            }
            for (const auto& mention : room.mentions) {
                const std::string candidate = "L|" + std::string(1, entry.code) + "|40";
                check(test::gate(mention + " свет 40", candidate) == candidate, "configured mention is explicit old gate evidence");
                check(test::gate(mention + " свет 40", "L|" + std::string(1, entry.code == 'K' ? 'B' : 'K') + "|40") == "U", "mention cannot authorize other room");
            }
            check(!firefly::legacy::ActionValidator::validate("L|" + std::string(1, entry.code) + "|101"), "101 unsupported");
            check(!firefly::legacy::ActionValidator::validate("L|" + std::string(1, entry.code) + "|-1"), "negative unsupported");
        }
        const auto& door = config.devices.at(catalog->deviceIndex().at(config.compatibility.doorTargetId));
        check(door.id == "device_entrance_door" && door.kind == firefly::config::DeviceKind::EntranceDoor && !door.roomId, "same one roomless entrance target");
        const auto& opening = config.actions.at(catalog->actionIndex().at(config.compatibility.doorActionId));
        check(opening.operation == firefly::config::Operation::OpenEntranceDoor && opening.parameters.empty(), "same door action and no invented parameters");
        check(firefly::legacy::toCompact(firefly::legacy::OpenEntranceDoor{}) == "D", "legacy D unchanged");
        check(!test::parse("открой входную дверь"), "door route remains fallback");
        for (const auto* verb : {"открой", "откройте", "открыть", "открывай", "открывайте"})
            check(test::gate(std::string(verb) + " входную дверь", "D") == "D", "same explicit door forms");
        check(test::gate("Открыть дверь на входе", "D") == "D", "frozen DEV entrance form preserved");
        check(test::gate("открытвер на входе", "D") == "U", "recorded M4 false reject remains uncorrected");
        check(test::gate("открой дверь", "D") == "U", "missing entrance fails closed");
        check(!firefly::legacy::ActionValidator::validate("U"), "U is not an action");
        std::cout << "ConfigShadow: " << checks << " checks passed; 404 light values and one entrance target agree\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Shadow failure after " << checks << " checks: " << error.what() << '\n'; return 1;
    }
}
