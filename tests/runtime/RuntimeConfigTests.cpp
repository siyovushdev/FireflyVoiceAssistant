#include "firefly/runtime/RuntimeConfig.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/config/ConfigValidator.h"
#include "firefly/ActionValidator.h"
#include "firefly/CommandEngine.h"
#include "firefly/DeterministicParser.h"
#include "firefly/SafetyGate.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <sstream>
#include <type_traits>
#include <unistd.h>

namespace {
using namespace firefly;
using namespace firefly::runtime;
static_assert(std::is_const_v<config::Catalog::element_type>);
static_assert(!std::is_copy_assignable_v<DomainView> && !std::is_move_assignable_v<DomainView>);
static_assert(!std::is_copy_assignable_v<RuntimeConfig> && !std::is_move_assignable_v<RuntimeConfig>);
static_assert(std::is_same_v<decltype(std::declval<DomainView>().room("id")), const config::Room*>);
static_assert(std::is_same_v<decltype(std::declval<RuntimeConfig>().domain()), const DomainView&>);

int checks = 0;
void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
struct Scratch {
    std::string path;
    Scratch() {
        char pattern[] = "/tmp/firefly-runtime-config-XXXXXX";
        const auto* created = mkdtemp(pattern);
        if (!created) throw std::runtime_error("mkdtemp failed");
        path = created;
    }
    ~Scratch() { std::error_code error; std::filesystem::remove_all(path, error); }
};
void write(const std::string& path, const std::string& bytes) {
    std::ofstream output(path);
    output << bytes;
    output.close();
    if (!output) throw std::runtime_error("Cannot write test fixture");
}
struct ExpectedRoom {
    Room room;
    char code;
    std::string id, target;
    std::vector<std::string> mentions, locations;
};
// Independent test oracle; no runtime code imports these fixtures.
const std::vector<ExpectedRoom> expectedRooms{
    {Room::Kitchen, 'K', "room_kitchen", "device_kitchen_light", {"кухня", "кухне", "кухню", "кухни", "кухней"}, {"на кухне"}},
    {Room::Bedroom, 'B', "room_bedroom", "device_bedroom_light", {"спальня", "спальне", "спальню", "спальни", "спальней"}, {"в спальне"}},
    {Room::LivingRoom, 'G', "room_living", "device_living_light", {"гостиная", "гостиной", "гостиную", "зал", "зале", "зала", "залу", "залом"}, {"в гостиной", "в зале"}},
    {Room::Corridor, 'C', "room_corridor", "device_corridor_light", {"коридор", "коридоре", "коридора", "коридору", "коридором"}, {"в коридоре"}}
};
class DoorLlm final : public LlmEngine {
public:
    std::string infer(std::string_view) override { return "D"; }
};
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Repository root required");
        const auto sample = config::ConfigLoader::fromFile(std::string(argv[1]) + "/config/current-m4.json");
        Scratch scratch;
        const auto file = scratch.path + "/current.json";
        write(file, sample->canonicalJson());
        const RuntimeConfig owner(file);
        const auto& domain = owner.domain();
        check(domain.snapshot()->contentHash() == sample->contentHash(), "startup loads chosen revision/hash");
        check(domain.snapshot()->configuration().rooms.size() == 4, "exact room count");
        check(domain.snapshot()->configuration().devices.size() == 5, "exact device count");
        check(domain.snapshot()->mentionIndex().size() == 23, "all current mentions");
        check(domain.snapshot()->locationIndex().size() == 5, "all current locations");
        owner.comparison().requireEquivalent();
        check(owner.comparison().equivalent(), "startup equivalence");
        check(compareLegacyDomain(domain).checks == owner.comparison().checks, "deterministic probe count");
        check(compareLegacyDomain(domain).mismatches == owner.comparison().mismatches, "deterministic comparison");
        std::ostringstream diagnostic, repeat;
        owner.writeDiagnostics(diagnostic); owner.writeDiagnostics(repeat);
        check(diagnostic.str() == repeat.str() && diagnostic.str().find("mismatches=0 authoritative=legacy") != std::string::npos,
              "deterministic shadow diagnostic");

        for (const auto& expected : expectedRooms) {
            const auto* room = domain.room(expected.id);
            const auto* target = domain.device(expected.target);
            check(room && room->id == expected.id, "canonical room ID");
            check(target && target->roomId == expected.id && target->kind == config::DeviceKind::Light, "canonical light target");
            check(domain.legacyLightTarget(expected.code) == target, "legacy lookup from snapshot");
            check(domain.brightnessBounds(target->id) == std::make_pair<std::int64_t, std::int64_t>(0, 100), "effective bounds");
            for (std::int64_t value : {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()})
                check(!domain.allowsBrightness(target->id, value), "extreme values rejected without overflow");
            for (int value = -1; value <= 101; ++value) {
                const auto compact = "L|" + std::string(1, expected.code) + "|" + std::to_string(value);
                check(domain.allowsBrightness(target->id, value) == ActionValidator::validate(compact).has_value(), "every brightness/boundary equals old validator");
                if (value < 0 || value > 100) continue;
                check(toCompact(LightAction{expected.room, value}) == compact, "enum target identity");
                for (const auto& location : expected.locations) {
                    check(domain.roomLocation(location) == room, "every location resolves");
                    check(DeterministicParser::parse(location + " свет " + std::to_string(value)) == compact, "whole production range per location");
                }
            }
            for (const auto& alias : expected.mentions) {
                check(domain.roomMention(alias) == room, "every mention resolves");
                for (const auto& candidate : expectedRooms) {
                    const auto compact = "L|" + std::string(1, candidate.code) + "|40";
                    check((SafetyGate::apply(alias + " свет 40", compact) == compact) == (candidate.id == room->id), "all aliases vs all production rooms");
                }
            }
        }
        for (int byte = 0; byte <= 255; ++byte) {
            const char code = static_cast<char>(static_cast<unsigned char>(byte));
            check((domain.legacyLightTarget(code) != nullptr) == ActionValidator::validate("L|" + std::string(1, code) + "|40").has_value(), "entire code space");
        }
        for (const auto& unknown : {"", "room_bathroom", "kitchen", "Кухня", "ROOM_KITCHEN", "device_missing"}) {
            check(!domain.room(unknown) && !domain.device(unknown), "unsupported canonical IDs");
            check(!domain.brightnessBounds(unknown) && !domain.allowsBrightness(unknown, 40), "unsupported target is unavailable");
        }
        for (const auto& unknown : {"", "ванна", "ванная", "КУХНЯ", " кухня", "свет на кухне", "открытвер на входе"})
            check(!domain.roomMention(unknown) && !domain.roomLocation(unknown), "exact aliases only; no new normalization/repair");
        const auto* door = domain.entranceTarget();
        check(door && door->id == "device_entrance_door" && door->kind == config::DeviceKind::EntranceDoor && !door->roomId, "one roomless entrance target");
        check(domain.entranceAvailable() && !domain.brightnessBounds(door->id), "entrance capability differs from light");
        const auto* opening = domain.effectiveAction(door->id, "action_open_entrance");
        check(opening && !opening->mandatoryPolicies.empty() && opening->operation == config::Operation::OpenEntranceDoor, "mandatory policy metadata retained");
        check(!domain.effectiveAction(door->id, "action_set_brightness") && !domain.effectiveAction("unknown", "unknown"), "unsupported operation relation");

        // Snapshot holds owned values and has no refresh path. Neither replacement
        // nor deletion of the source file changes the process-lifetime object.
        auto changed = sample->configuration();
        changed.revision = 2; changed.system.name = "Другой дом";
        const auto renamed = config::ConfigValidator::validate(changed);
        write(file, renamed->canonicalJson());
        check(domain.snapshot()->configuration().revision == 1 && domain.snapshot()->contentHash() == sample->contentHash(), "file replacement cannot mutate snapshot");
        const RuntimeConfig nextProcess(file);
        check(nextProcess.domain().snapshot()->configuration().revision == 2 && nextProcess.comparison().equivalent(), "explicit new owner sees renamed revision");
        std::filesystem::remove(file);
        check(domain.room("room_kitchen") && domain.snapshot()->canonicalJson() == sample->canonicalJson(), "file removal cannot mutate snapshot");
        std::weak_ptr<const config::ValidatedCatalog> lifetime;
        {
            write(file, sample->canonicalJson());
            const RuntimeConfig temporary(file);
            lifetime = temporary.domain().snapshot();
            check(!lifetime.expired(), "owner holds snapshot");
        }
        check(lifetime.expired(), "snapshot released with its last owner");
        bool nullRejected = false;
        try { DomainView invalid(nullptr); } catch (const std::invalid_argument&) { nullRejected = true; }
        check(nullRejected, "no empty domain view");

        const std::vector<std::function<void(config::Configuration&)>> drifts{
            [](auto& c) { c.rooms[0].mentions.pop_back(); },
            [](auto& c) { c.rooms[0].mentions.push_back("опочивальня"); },
            [](auto& c) { for (auto& room : c.rooms) if (room.locations.size() > 1) room.locations.pop_back(); },
            [](auto& c) { for (auto& a : c.actions) if (!a.parameters.empty()) a.parameters[0].maximum = 99; },
            [](auto& c) { for (auto& a : c.actions) if (!a.parameters.empty()) a.parameters[0].minimum = 1; },
            [](auto& c) { c.devices[0].enabled = false; },
            [](auto& c) { for (auto& a : c.actions) if (a.operation == config::Operation::OpenEntranceDoor) a.enabled = false; },
            [](auto& c) { c.restrictions.push_back({{"deny_entrance", "Запрет"}, "device_entrance_door", "action_open_entrance"}); },
            [](auto& c) { auto extra = c.devices[0]; extra.id = "extra_light"; c.devices.push_back(extra); }
        };
        DoorLlm llm;
        CommandEngine oldEngine(llm);
        for (const auto& drift : drifts) {
            auto candidate = sample->configuration(); drift(candidate);
            auto catalog = config::ConfigValidator::validate(candidate);
            DomainView view(catalog);
            const auto comparison = compareLegacyDomain(view);
            check(!comparison.equivalent(), "valid drift detected");
            check(comparison.mismatches == compareLegacyDomain(view).mismatches, "stable mismatch details");
            bool loud = false;
            try { comparison.requireEquivalent(); } catch (const std::runtime_error&) { loud = true; }
            check(loud, "test assertion fails loudly on drift");
            write(file, catalog->canonicalJson());
            const RuntimeConfig shadowOnly(file);
            check(!shadowOnly.comparison().equivalent(), "valid drift retained, never silently repaired");
            check(shadowOnly.domain().snapshot()->contentHash() == catalog->contentHash(), "chosen mismatching snapshot retained");
            check(oldEngine.process("на кухне свет 100").compact() == "L|K|100", "production light result unaffected");
            check(oldEngine.process("Открой входную дверь").compact() == "D", "production entrance result unaffected");
            check(oldEngine.process("открытвер на входе").compact() == "U", "recorded rejection unaffected");
        }
        // Invalid files never construct a partially usable runtime owner.
        for (const auto& bytes : {std::string("{"), std::string("{}"), std::string("{\"schema_version\":9}"),
                                 std::string("{\"schema_version\":1,\"schema_version\":1}")}) {
            write(file, bytes);
            std::unique_ptr<RuntimeConfig> invalid;
            bool rejected = false;
            try { invalid = std::make_unique<RuntimeConfig>(file); } catch (const config::ConfigError&) { rejected = true; }
            check(rejected && !invalid, "invalid startup has no partial snapshot");
        }
        std::filesystem::remove(file);
        bool missingRejected = false;
        try { RuntimeConfig missing(file); } catch (const config::ConfigError&) { missingRejected = true; }
        check(missingRejected, "missing file has no fallback");
        check(domain.snapshot()->contentHash() == sample->contentHash(), "other owners/failures never change existing snapshot");
        std::cout << "RuntimeConfig: " << checks << " checks passed; startup probes=" << owner.comparison().checks
                  << "; mismatches=0; 404 light values, 23 mentions, 5 locations, one entrance target\n";
    } catch (const std::exception& error) {
        std::cerr << "RuntimeConfig failed after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
