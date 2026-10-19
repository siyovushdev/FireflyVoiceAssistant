// CPU-only direct executor fixture for real isolated DNS acceptance.
#include "firefly/config/ConfigLoader.h"
#include "firefly/execution/ActionExecutor.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <future>
#include <map>
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        firefly::execution::BoundDeviceExecutor executor(firefly::config::ConfigLoader::fromFile(argv[1]));
        const auto execute = [&](const std::string& target) {
            const auto result = executor.execute(firefly::actions::GenericAction("action_on", target, {}));
            const auto state = executor.production().device(target);
            return nlohmann::json{{"success",result.success},{"error",result.error},
                {"knowledge",firefly::execution::DeviceStateStore::knowledgeName(state->properties.at("power").knowledge)}};
        };
        std::map<std::string, std::future<nlohmann::json>> pending;
        std::cout << nlohmann::json{{"ready",true}} << std::endl;
        std::string line;
        while (std::getline(std::cin, line)) {
            const auto command = nlohmann::json::parse(line);
            const auto op = command.value("op", "exec");
            if (op == "close") break;
            if (op == "launch") {
                const auto target = command.at("target").get<std::string>();
                pending.emplace(command.at("id").get<std::string>(), std::async(std::launch::async, [&, target] { return execute(target); }));
                std::cout << nlohmann::json{{"launched",true}} << std::endl;
            } else if (op == "collect") {
                const auto id = command.at("id").get<std::string>();
                std::cout << pending.at(id).get() << std::endl;
                pending.erase(id);
            } else std::cout << execute(command.at("target").get<std::string>()) << std::endl;
        }
        return 0;
    } catch (const std::exception&) { return 1; }
}
