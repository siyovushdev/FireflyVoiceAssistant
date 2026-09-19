#include "firefly/llm/StructuredOutput.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/config/ConfigValidator.h"
#include <iostream>
#include <limits>
#include <vector>

using namespace firefly;
using namespace firefly::actions;
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Repository root required");
        const runtime::DomainView domain(config::ConfigLoader::fromFile(std::string(argv[1]) + "/config/current-m4.json"));
        const llm::StructuredDecoder decoder(domain);
        int checks = 0, actions = 0, rejected = 0;
        const auto check = [&](bool condition) { ++checks; if (!condition) throw std::runtime_error("Structured assertion " + std::to_string(checks)); };
        const auto rejects = [&](const std::string& bytes) {
            const auto result = decoder.decode(bytes);
            check(!result.valid() && std::holds_alternative<Unsupported>(result.value));
            ++rejected;
        };
        for (const auto& target : domain.snapshot()->configuration().devices) {
            for (const auto& actionId : target.actionIds) {
                const auto* definition = domain.effectiveAction(target.id, actionId);
                const bool light = definition->brightnessBounds.has_value();
                for (std::int64_t value = 0; value <= (light ? 100 : 0); ++value) {
                    GenericAction::Arguments arguments;
                    if (light) arguments.emplace("brightness", IntegerArgument(value));
                    const CandidateResult expected{GenericAction(actionId, target.id, arguments)};
                    const auto json = llm::structuredResult(expected);
                    const auto decoded = decoder.decode(json);
                    check(decoded.status == llm::DecodeStatus::Action && decoded.value == expected);
                    check(decoder.decode(" \t\r\n" + json + "\n").value == expected);
                    ++actions;
                    for (std::size_t end = 0; end < json.size(); ++end) rejects(json.substr(0, end));
                    for (const auto* prefix : {"answer: ", "```json\n", "null", "{}", "\xef\xbb\xbf"}) rejects(prefix + json);
                    for (const auto* suffix : {" text", "\n```", "{}", "false", "// comment"}) rejects(json + suffix);
                }
            }
        }
        check(actions == 405);
        const auto unsupported = decoder.decode(R"({"unsupported":true})");
        check(unsupported.valid() && unsupported.status == llm::DecodeStatus::Unsupported &&
              std::holds_alternative<Unsupported>(unsupported.value));
        const std::string prefix = R"({"actionId":"action_set_brightness","targetId":"device_kitchen_light","arguments":)";
        for (const std::string arguments : {"{}", "[]", "null", "true", R"({"brightness":"40"})",
                R"({"brightness":40.0})", R"({"brightness":4e1})", R"({"brightness":true})",
                R"({"brightness":null})", R"({"brightness":[]})", R"({"brightness":{}})",
                R"({"brightness":-1})", R"({"brightness":101})", R"({"brightness":9223372036854775808})",
                R"({"brightness":-9223372036854775809})", R"({"brightness":18446744073709551616})",
                R"({"brightness":40,"extra":1})", R"({"brightness":40,"brightness":50})",
                R"({"brightness":40,"\u0062rightness":50})", R"({"Brightness":40})"}) rejects(prefix + arguments + "}");
        for (const std::string bytes : {"", "D", "U", "L|K|40", "[]", "null", "true", "40", "{", "{}",
                R"({"unsupported":false})", R"({"unsupported":1})", R"({"unsupported":"true"})",
                R"({"unsupported":true,"unsupported":true})", R"({"unsupported":true,"arguments":{}})",
                R"({"unsupported":true,"actionId":"action_open_entrance","targetId":"device_entrance_door","arguments":{}})",
                R"({"actionId":"missing","targetId":"device_entrance_door","arguments":{}})",
                R"({"actionId":"action_open_entrance","targetId":"missing","arguments":{}})",
                R"({"actionId":"action_open_entrance","targetId":"device_kitchen_light","arguments":{}})",
                R"({"actionId":"action_set_brightness","targetId":"device_entrance_door","arguments":{"brightness":40}})",
                R"({"actionId":"action_open_entrance","targetId":"device_entrance_door","arguments":{"brightness":0}})",
                R"({"actionId":"action_open_entrance","targetId":"device_entrance_door"})",
                R"({"actionId":"action_open_entrance","targetId":"device_entrance_door","arguments":{},"extra":1})",
                R"({"actionId":"action_open_entrance","actionId":"action_open_entrance","targetId":"device_entrance_door","arguments":{}})",
                R"({"actionId":null,"targetId":"device_entrance_door","arguments":{}})"}) rejects(bytes);
        rejects(std::string(4097, ' '));
        rejects(std::string(1000, '[') + std::string(1000, ']'));
        const auto canonical = prefix + R"({"brightness":40}})";
        for (int byte = 0; byte < 256; ++byte) {
            const auto decoded = decoder.decode(std::string(1, static_cast<char>(byte)) + canonical);
            if (byte == 9 || byte == 10 || byte == 13 || byte == 32) check(decoded.valid());
            else check(!decoded.valid());
        }
        // Fixed-seed malformed inputs: no external fuzzer/dependency or randomness.
        std::uint32_t state = 0x3588;
        for (int i = 0; i < 10000; ++i) {
            std::string bytes = "{";
            for (int j = 0; j < i % 80; ++j) {
                state ^= state << 13; state ^= state >> 17; state ^= state << 5;
                bytes += static_cast<char>(state & 255);
            }
            rejects(bytes); // No complete required three-member object can fit this random fixture grammar.
        }
        auto narrow = domain.snapshot()->configuration();
        for (auto& action : narrow.actions) for (auto& parameter : action.parameters) { parameter.minimum = 10; parameter.maximum = 90; }
        const runtime::DomainView narrowed(config::ConfigValidator::validate(narrow));
        const llm::StructuredDecoder narrower(narrowed);
        for (std::int64_t brightness : {0, 9, 10, 40, 90, 91, 100}) {
            const auto json = llm::structuredResult(GenericAction("action_set_brightness", "device_kitchen_light", {{"brightness", IntegerArgument(brightness)}}));
            check(narrower.decode(json).valid() == (brightness >= 10 && brightness <= 90));
            check(decoder.decode(json).valid());
        }
        std::cout << "StructuredOutput: " << checks << " checks; " << actions << " complete actions; "
                  << rejected << " malformed/unsupported fixtures rejected\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
