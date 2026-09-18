#include "firefly/CommandEngine.h"
#include "firefly/DeterministicParser.h"
#include "firefly/SafetyGate.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/config/ConfigValidator.h"
#include "firefly/runtime/RuntimeConfig.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <type_traits>

namespace {
using namespace firefly;
int checks = 0;
void check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}
class FakeLlm final : public LlmEngine {
public:
    int calls = 0;
    std::string raw = "D", original;
    std::string infer(std::string_view input) override {
        ++calls;
        original = input;
        return raw;
    }
};
class CountedEntrance final : public EntranceDoorDomain {
public:
    explicit CountedEntrance(const runtime::DomainView& view) : view_(view) {}
    mutable int calls = 0;
    std::optional<EntranceDoorTarget> resolveEntranceDoor() const override {
        ++calls;
        return view_.resolveEntranceDoor();
    }
private:
    const runtime::DomainView& view_;
};
class MissingEntrance final : public EntranceDoorDomain {
public:
    std::optional<EntranceDoorTarget> resolveEntranceDoor() const override { return std::nullopt; }
};
class FailedEntrance final : public EntranceDoorDomain {
public:
    std::optional<EntranceDoorTarget> resolveEntranceDoor() const override {
        throw std::runtime_error("entrance lookup failed");
    }
};
config::Device& selectedDoor(config::Configuration& value) {
    for (auto& device : value.devices)
        if (device.id == value.compatibility.doorTargetId) return device;
    throw std::runtime_error("test fixture lost entrance");
}
void rejectedConfiguration(const config::Configuration& value) {
    bool threw = false;
    try { (void)config::ConfigValidator::validate(value); }
    catch (const config::ConfigError&) { threw = true; }
    check(threw, "invalid target cannot publish a snapshot or use a legacy fallback");
}
void requireMismatch(const runtime::ShadowComparison& comparison) {
    bool threw = false;
    try { comparison.requireEquivalent(); } catch (const std::runtime_error&) { threw = true; }
    check(threw, "shadow mismatch must fail loudly in tests");
}
static_assert(std::is_base_of_v<EntranceDoorDomain, runtime::DomainView>);
static_assert(!std::is_copy_assignable_v<runtime::DomainView>);
static_assert(!std::is_copy_assignable_v<runtime::RuntimeConfig>);
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Repository root required");
        const std::string root = argv[1];
        const runtime::RuntimeConfig owner(root + "/config/current-m4.json");
        const auto sample = owner.domain().snapshot();
        const auto canonical = sample->canonicalJson();
        owner.comparison().requireEquivalent();
        owner.brightnessComparison().requireEquivalent();
        owner.entranceComparison().requireEquivalent();
        check(owner.comparison().checks == 1364 && owner.brightnessComparison().checks == 668,
              "prior room/range comparison coverage unchanged");
        check(owner.entranceComparison().checks == 2, "entrance identity and singleton profile probes");
        const auto target = owner.domain().resolveEntranceDoor();
        check(target && target->deviceId == "device_entrance_door" && target->displayName == "Входная дверь",
              "independent frozen entrance identity and label fixture");
        check(!owner.domain().device("unknown_door"), "unknown device is not an entrance substitute");
        std::ostringstream diagnostics;
        owner.writeDiagnostics(diagnostics);
        check(diagnostics.str().find("ENTRANCE_DOMAIN authoritative=config checks=2 mismatches=0 legacy_shadow=matched\n") != std::string::npos,
              "production entrance authority and comparison diagnosed");

        std::vector<std::pair<std::string, std::string>> matrix{
            {"дверь", "U"}, {"открой дверь", "U"},
            {"открой входную дверь", "D"}, {"открыть дверь на входе", "D"},
            {"ОТКРОЙ дверь НА\tВХОДЕ!", "D"}, {"  ОТКРОЙ ВХОДНУЮ ДВЕРЬ!  ", "D"},
            {"двер", "U"}, {"входная дверь", "U"}, {"открой входную", "U"},
            {"закрой входную дверь", "U"}, {"Разблокируй входную дверь", "U"},
            {"Открой дверь в спальню", "U"}, {"открой дверь в ванную", "U"},
            {"открой входную тверь", "U"}, {"аткрой входную дверь", "U"},
            {"открытая входная дверь", "U"}, {"открой вход", "U"},
            {"дверь на входе", "U"}, {"открой на входе", "U"},
            {"открой тверь на входе", "U"}, {"открой дверь на входеже", "U"},
            {"открой дверь страна входе", "U"}, {"открой дверь на другом входе", "U"},
            {"открытвер на входе", "U"}};
        for (const std::string intent : {"открой", "откройте", "открыть", "открывай", "открывайте"}) {
            matrix.emplace_back(intent + " входную дверь", "D");
            matrix.emplace_back(intent + " дверь на входе", "D");
        }
        // Use the original frozen labels, including related negative DEV cases.
        std::ifstream dev(root + "/tests/data/dev.tsv");
        check(bool(dev), "frozen DEV readable");
        std::string line;
        int devDoors = 0;
        while (std::getline(dev, line)) {
            std::istringstream fields(line);
            std::string id, category, expected, input;
            std::getline(fields, id, '\t'); std::getline(fields, category, '\t');
            std::getline(fields, expected, '\t'); std::getline(fields, input);
            if (input.find("двер") != std::string::npos) {
                matrix.emplace_back(input, expected);
                ++devDoors;
            }
        }
        check(devDoors == 6, "all six existing door-related DEV cases included");
        std::cout << "Door safety matrix: " << matrix.size() << " rows, " << devDoors << " frozen DEV rows\n";

        // Valid schema-level metadata edits must reach the actual production
        // result. Stable IDs cannot be remapped under legacy_m4_v1; do not relax it.
        CommandResult retained{};
        for (const std::string name : {"Входная дверь", "Главный вход — тест", "дверь"}) {
            auto changed = sample->configuration();
            selectedDoor(changed).name = name;
            std::reverse(changed.devices.begin(), changed.devices.end());
            const auto snapshot = config::ConfigValidator::validate(changed);
            const runtime::DomainView domain(snapshot);
            runtime::compareLegacyEntranceDoor(domain).requireEquivalent();
            const DeterministicParser parser(domain);
            parser.requireLegacyRoomDomain();
            CountedEntrance entrance(domain);
            FakeLlm llm;
            CommandEngine engine(llm, parser, domain, entrance);
            for (const auto& [input, expected] : matrix) {
                check(!parser.parseChecked(input), "door routing remains unresolved in the parser");
                for (const std::string raw : {"D", " \nD\t", "U", "", "D|device_entrance_door", "D|unknown_door"}) {
                    llm.raw = raw;
                    const bool doorCandidate = raw == "D" || raw == " \nD\t";
                    const auto final = doorCandidate ? expected : "U";
                    const int before = llm.calls, lookups = entrance.calls;
                    const auto result = engine.process(input);
                    check(result.compact() == final, "gate/codec/domain composition: " + input + " / " + raw);
                    check(result.path == CommandPath::LlmFallback && llm.calls == before + 1,
                          "same inference route, no domain retries");
                    check(llm.original == input && result.rawOutput == raw, "original and raw text unchanged");
                    check(entrance.calls == lookups + (final == "D" ? 1 : 0), "resolve only once after safety/strict decoding");
                    check(result.entranceTarget.has_value() == (final == "D"), "no target on rejected candidates");
                    if (result.entranceTarget) {
                        check(result.entranceTarget->deviceId == snapshot->configuration().compatibility.doorTargetId,
                              "production target comes from configured mapping");
                        check(result.entranceTarget->displayName == name, "config metadata change reaches CommandResult");
                        check(std::holds_alternative<OpenEntranceDoor>(*result.action), "original Action/protocol retained");
                        retained = result;
                    }
                }
                check(SafetyGate::apply(input, "D") == expected, "real frozen SafetyGate is still the policy authority");
            }
            for (const auto& [location, code] : std::vector<std::pair<std::string,char>>{
                     {"на кухне",'K'}, {"в спальне",'B'}, {"в гостиной",'G'}, {"в зале",'G'}, {"в коридоре",'C'}}) {
                for (int value : {0,40,100}) {
                    const auto text = location + " свет " + std::to_string(value);
                    const int calls = entrance.calls;
                    const auto result = engine.process(text);
                    check(result.compact() == "L|" + std::string(1,code) + "|" + std::to_string(value),
                          "room/range composition preserved");
                    check(!result.entranceTarget && entrance.calls == calls, "light must never resolve a door");
                }
            }
            for (const std::string candidate : {"U", "L|K|50", "L|K|0", "D"}) {
                llm.raw = candidate;
                const int calls = entrance.calls;
                const auto result = engine.process("На кухне выключи свет на 50 процентов");
                check(result.compact() == "U" && !result.entranceTarget && entrance.calls == calls,
                      "6.4.1 contradiction remains terminal before any target lookup");
            }
        }
        check(retained.compact() == "D" && retained.entranceTarget->displayName == "дверь",
              "result owns metadata after engine/domain destruction");

        const DeterministicParser parser(owner.domain());
        FakeLlm llm;
        MissingEntrance missing;
        FailedEntrance failed;
        for (const EntranceDoorDomain* resolver : std::vector<const EntranceDoorDomain*>{&missing, &failed}) {
            CommandEngine engine(llm, parser, owner.domain(), *resolver);
            for (const auto& text : {"открой входную дверь", "открыть дверь на входе"}) {
                const int before = llm.calls;
                const auto result = engine.process(text);
                check(result.compact() == "U" && !result.entranceTarget && !result.error.empty(),
                      "missing/failed resolver cannot borrow the legacy entrance");
                check(result.rawOutput == "D" && llm.calls == before + 1, "domain failure has no repair or retry");
            }
            check(engine.process("на кухне свет 40").compact() == "L|K|40", "door failure does not affect lights");
        }
        // Missing/unsupported/misassigned definitions cannot create a snapshot.
        for (int defect = 0; defect < 7; ++defect) {
            auto changed = sample->configuration();
            auto& door = selectedDoor(changed);
            if (defect == 0) changed.compatibility.doorTargetId = "unknown_door";
            if (defect == 1) changed.compatibility.doorTargetId.clear();
            if (defect == 2) door.actionIds.clear();
            if (defect == 3) door.roomId = "room_kitchen";
            if (defect == 4) changed.compatibility.doorTargetId = "device_kitchen_light";
            if (defect == 5) {
                door.id = "device_other_entrance";
                changed.compatibility.doorTargetId = door.id;
            }
            if (defect == 6) changed.devices.erase(std::remove_if(changed.devices.begin(), changed.devices.end(),
                [&](const auto& device) { return device.id == changed.compatibility.doorTargetId; }), changed.devices.end());
            rejectedConfiguration(changed);
        }
        // A valid additional stored entrance is detectable legacy profile drift.
        // It is never selected by order/name/kind, nor addressable through D.
        auto extra = sample->configuration();
        auto other = selectedDoor(extra);
        other.id = "device_aaa_other_entrance";
        other.name = "Иной вход (тест)";
        extra.devices.insert(extra.devices.begin(), other);
        const runtime::DomainView extraDomain(config::ConfigValidator::validate(extra));
        const auto mismatch = runtime::compareLegacyEntranceDoor(extraDomain);
        check(mismatch.checks == 2 && mismatch.mismatches == std::vector<std::string>{"entrance_entity_set"},
              "exact deterministic mismatch for valid test-only entrance entity drift");
        check(mismatch.mismatches == runtime::compareLegacyEntranceDoor(extraDomain).mismatches,
              "ordered mismatch repeatability");
        requireMismatch(mismatch);
        const DeterministicParser extraParser(extraDomain);
        CommandEngine extraEngine(llm, extraParser, extraDomain, extraDomain);
        const auto selected = extraEngine.process("открой входную дверь");
        check(selected.compact() == "D" && selected.entranceTarget->deviceId == target->deviceId,
              "shadow drift never substitutes another target or vetoes the configured mapping");

        // Permission activation is a separate migration, as in 6.5. Preserve
        // existing observational behavior rather than silently activating it.
        auto disabled = sample->configuration();
        selectedDoor(disabled).enabled = false;
        for (auto& action : disabled.actions)
            if (action.operation == config::Operation::OpenEntranceDoor) action.enabled = false;
        config::DenyAction deny;
        deny.id = "deny_entrance"; deny.name = "Запрет (тест)";
        deny.deviceId = disabled.compatibility.doorTargetId;
        deny.actionId = disabled.compatibility.doorActionId;
        disabled.restrictions.push_back(deny);
        const runtime::DomainView disabledDomain(config::ConfigValidator::validate(disabled));
        check(!disabledDomain.entranceAvailable(), "existing permission-aware shadow helper retained");
        runtime::compareLegacyEntranceDoor(disabledDomain).requireEquivalent();
        const DeterministicParser disabledParser(disabledDomain);
        CommandEngine disabledEngine(llm, disabledParser, disabledDomain, disabledDomain);
        check(disabledEngine.process("открой входную дверь").compact() == "D", "identity cutover does not migrate permissions");
        check(disabledEngine.process("открой дверь").compact() == "U", "metadata cannot weaken intent policy");
        check(sample->canonicalJson() == canonical && config::ConfigLoader::fromFile(root + "/config/current-m4.json")->canonicalJson() == canonical,
              "immutable sample and production file unchanged");
        std::cout << "Entrance door cutover: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Entrance door cutover: " << error.what() << '\n';
        return 1;
    }
}
