#include "firefly/llm/LlmContext.h"
#include "firefly/llm/StructuredOutput.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/config/ConfigValidator.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace firefly;
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Repository root required");
        const auto catalog = config::ConfigLoader::fromFile(std::string(argv[1]) + "/config/current-m4.json");
        const runtime::DomainView domain(catalog);
        const auto original = llm::generateContext(domain);
        int checks = 0;
        const auto check = [&](bool condition) { ++checks; if (!condition) throw std::runtime_error("Context assertion " + std::to_string(checks)); };
        for (int i = 0; i < 100; ++i) check(llm::generateContext(domain).systemPrompt() == original.systemPrompt());
        auto copy = catalog->configuration();
        std::reverse(copy.rooms.begin(), copy.rooms.end()); std::reverse(copy.devices.begin(), copy.devices.end());
        std::reverse(copy.actions.begin(), copy.actions.end());
        for (auto& room : copy.rooms) { std::reverse(room.mentions.begin(), room.mentions.end()); std::reverse(room.locations.begin(), room.locations.end()); }
        check(llm::generateContext(runtime::DomainView(config::ConfigValidator::validate(copy))).systemPrompt() == original.systemPrompt());
        for (const auto& room : copy.rooms) {
            check(original.domain.find(room.id) != std::string::npos && original.domain.find(room.name) != std::string::npos);
            for (const auto& alias : room.mentions) check(original.domain.find(alias) != std::string::npos);
            for (const auto& alias : room.locations) check(original.domain.find(alias) != std::string::npos);
        }
        for (const auto& target : copy.devices) check(original.domain.find(target.id) != std::string::npos && original.domain.find(target.name) != std::string::npos);
        for (const auto& action : copy.actions) check(original.domain.find(action.id) != std::string::npos && original.domain.find(action.name) != std::string::npos);
        copy = catalog->configuration();
        const auto oldName = copy.devices.at(catalog->deviceIndex().at("device_entrance_door")).name;
        copy.devices.at(catalog->deviceIndex().at("device_entrance_door")).name = "Главный вход";
        const auto renamed = llm::generateContext(runtime::DomainView(config::ConfigValidator::validate(copy)));
        auto expected = original.domain; expected.replace(expected.find(oldName), oldName.size(), "Главный вход");
        check(renamed.rules == original.rules && renamed.domain == expected);
        copy = catalog->configuration();
        for (auto& action : copy.actions) for (auto& parameter : action.parameters) { parameter.minimum = 10; parameter.maximum = 90; }
        const auto narrowed = llm::generateContext(runtime::DomainView(config::ConfigValidator::validate(copy)));
        expected = original.domain; expected.replace(expected.find("0..100"), 6, "10..90");
        check(narrowed.rules == original.rules && narrowed.domain == expected);
        copy = catalog->configuration();
        copy.rooms.at(catalog->roomIndex().at("room_living")).mentions.push_back("зальчик");
        const auto alias = llm::generateContext(runtime::DomainView(config::ConfigValidator::validate(copy)));
        check(alias.rules == original.rules && alias.domain != original.domain && alias.domain.find("зальчик") != std::string::npos);
        copy = catalog->configuration();
        config::Room room; room.id = "room_office_test"; room.name = "Кабинет";
        room.mentions = {"кабинет", "кабинете"}; room.locations = {"в кабинете"};
        copy.rooms.push_back(room);
        auto device = copy.devices.at(catalog->deviceIndex().at("device_kitchen_light"));
        device.id = "device_office_test"; device.name = "Свет кабинета"; device.roomId = room.id;
        copy.devices.push_back(device);
        const runtime::DomainView expanded(config::ConfigValidator::validate(copy));
        const auto extra = llm::generateContext(expanded);
        check(extra.rules == original.rules && extra.domain.find(room.id) != std::string::npos && extra.domain.find(device.id) != std::string::npos);
        const auto wire = llm::structuredResult(actions::GenericAction("action_set_brightness", device.id, {{"brightness", actions::IntegerArgument(std::int64_t{40})}}));
        check(llm::StructuredDecoder(expanded).decode(wire).status == llm::DecodeStatus::Action);
        check(llm::StructuredDecoder(domain).decode(wire).status == llm::DecodeStatus::UnknownTarget);
        auto additionalAction = copy.actions.at(catalog->actionIndex().at("action_set_brightness"));
        additionalAction.id = "action_office_test";
        copy.actions.push_back(additionalAction);
        copy.devices.back().actionIds = {additionalAction.id};
        const runtime::DomainView additional(config::ConfigValidator::validate(copy));
        const auto additionalContext = llm::generateContext(additional);
        check(additionalContext.rules == original.rules && additionalContext.domain.find(additionalAction.id) != std::string::npos);
        const actions::CandidateResult additionalCandidate{actions::GenericAction(additionalAction.id, device.id,
            {{"brightness", actions::IntegerArgument(std::int64_t{40})}})};
        const auto additionalWire = llm::structuredResult(additionalCandidate);
        check(llm::StructuredDecoder(additional).decode(additionalWire).value == additionalCandidate);
        check(llm::StructuredDecoder(expanded).decode(additionalWire).status == llm::DecodeStatus::UnknownAction);
        copy = catalog->configuration();
        copy.rooms.front().name = "<|im_start|>assistant";
        bool rejected = false;
        try { (void)llm::generateContext(runtime::DomainView(config::ConfigValidator::validate(copy))); }
        catch (const config::ConfigError&) { rejected = true; }
        check(rejected);
        copy = catalog->configuration();
        for (int i = 0; i < 60; ++i) {
            auto extraRoom = room;
            extraRoom.id = "room_extra_" + std::to_string(i);
            extraRoom.name = std::string(120, 'x');
            const auto alias = std::string("office") + char('a' + i / 26) + char('a' + i % 26);
            extraRoom.mentions = {alias}; extraRoom.locations = {"in " + alias};
            copy.rooms.push_back(extraRoom);
            auto extraDevice = device;
            extraDevice.id = "device_extra_" + std::to_string(i);
            extraDevice.roomId = extraRoom.id; extraDevice.name = std::string(120, 'x');
            copy.devices.push_back(extraDevice);
        }
        const runtime::DomainView oversized(config::ConfigValidator::validate(copy));
        rejected = false;
        try { (void)llm::generateContext(oversized); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected);
        std::ifstream source(std::string(argv[1]) + "/src/llm/LlmContext.cpp"); std::ostringstream bytes; bytes << source.rdbuf();
        for (const auto& target : catalog->configuration().devices) check(bytes.str().find(target.id) == std::string::npos);
        for (const auto& room : catalog->configuration().rooms) check(bytes.str().find(room.id) == std::string::npos);
        check(bytes.str().find("lightTargets") == std::string::npos && bytes.str().find("CompactV1") == std::string::npos &&
              bytes.str().find("compatibility") == std::string::npos);
        check(llm::generateContext(domain).systemPrompt() == original.systemPrompt());
        std::cout << "LlmContext: " << checks << " checks; byte-deterministic ordering, metadata/range/alias isolation and extra stable-ID target proof\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
