// CPU-only direct executor fixture for real isolated DNS acceptance.
#include "firefly/config/ConfigLoader.h"
#include "firefly/execution/ActionExecutor.h"
#include <nlohmann/json.hpp>
#include <iostream>
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        firefly::execution::BoundDeviceExecutor executor(firefly::config::ConfigLoader::fromFile(argv[1]));
        std::cout << nlohmann::json{{"ready",true}} << std::endl;
        std::string line;
        while (std::getline(std::cin, line)) {
            const auto command = nlohmann::json::parse(line);
            if (command.value("op","") == "close") break;
            const auto target = command.at("target").get<std::string>();
            const auto result = executor.execute(firefly::actions::GenericAction("action_on", target, {}));
            const auto state = executor.production().device(target);
            std::cout << nlohmann::json{{"success",result.success},{"error",result.error},
                {"knowledge",firefly::execution::DeviceStateStore::knowledgeName(state->properties.at("power").knowledge)}} << std::endl;
        }
        return 0;
    } catch (const std::exception&) { return 1; }
}
