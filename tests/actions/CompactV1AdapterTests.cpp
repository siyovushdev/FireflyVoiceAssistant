#include "firefly/actions/CompactV1Adapter.h"
#include "firefly/Action.h"
#include "firefly/ActionValidator.h"
#include "firefly/SafetyGate.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/config/ConfigValidator.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace firefly;
using namespace firefly::actions;
int checks = 0;
int roundTrips = 0;
int malformed = 0;
void check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}
GenericAction light(std::string target, std::int64_t value, std::string action = "action_set_brightness") {
    return GenericAction(std::move(action), std::move(target), {{"brightness", IntegerArgument(value)}});
}
GenericAction decodedAction(const CompactDecodeResult& decoded) {
    check(std::holds_alternative<CandidateResult>(decoded), "decode must produce a candidate result");
    const auto& result = std::get<CandidateResult>(decoded);
    check(std::holds_alternative<GenericAction>(result), "decode must not produce Unsupported");
    return std::get<GenericAction>(result);
}
void encodedAs(const CompactV1Adapter& adapter, const GenericAction& action, const std::string& expected) {
    const auto result = adapter.encode(action);
    check(std::holds_alternative<std::string>(result), "expected encodable action: " + expected);
    check(std::get<std::string>(result) == expected, "canonical compact output: " + expected);
}
void encodeError(const CompactV1Adapter& adapter, const GenericAction& action, CompactV1Error expected) {
    const auto result = adapter.encode(action);
    check(std::holds_alternative<CompactV1Error>(result), "invalid/unrepresentable action must not encode, even as U");
    check(std::get<CompactV1Error>(result) == expected, "stable encoding failure category");
}
void invalidWire(const CompactV1Adapter& adapter, const std::string& wire) {
    const auto result = adapter.decode(wire);
    check(!ActionValidator::validate(wire), "independent frozen decoder rejects malformed wire");
    check(std::holds_alternative<CompactV1Error>(result), "malformed wire is not an Action or explicit U");
    check(std::get<CompactV1Error>(result) == CompactV1Error::InvalidSyntax, "malformed wire is a syntax failure");
    ++malformed;
}
void roundTrip(const CompactV1Adapter& adapter, const GenericAction& expected, const std::string& wire) {
    encodedAs(adapter, expected, wire);
    const auto decoded = decodedAction(adapter.decode(wire));
    check(decoded == expected, "exact stable-ID/typed-argument equality, not only wire equality");
    const auto encoded = std::get<std::string>(adapter.encode(expected));
    check(decodedAction(adapter.decode(encoded)) == expected, "Generic -> compact -> Generic");
    encodedAs(adapter, decoded, wire);
    const auto legacy = ActionValidator::validate(wire);
    check(legacy && toCompact(*legacy) == wire, "frozen independent canonical codec agrees");
    ++roundTrips;
}
void invalidConfiguration(config::Configuration value) {
    bool rejected = false;
    try { (void)config::ConfigValidator::validate(std::move(value)); }
    catch (const config::ConfigError&) { rejected = true; }
    check(rejected, "invalid mapping cannot create an adapter context or a legacy fallback");
}
static_assert(!std::is_default_constructible_v<CompactV1Adapter>);
static_assert(!std::is_copy_assignable_v<CompactV1Adapter>);
static_assert(!std::is_invocable_v<decltype(&CompactV1Adapter::encode), const CompactV1Adapter&, Unsupported>);
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Repository root required");
        const auto snapshot = config::ConfigLoader::fromFile(std::string(argv[1]) + "/config/current-m4.json");
        const auto canonical = snapshot->canonicalJson();
        const CompactV1Adapter adapter(snapshot);
        check(adapter.snapshot() == snapshot, "explicit immutable snapshot dependency");

        // Independent frozen expectation table belongs only to tests. Production
        // adapter mapping comes from the validated compatibility object.
        const std::array<std::pair<char, std::string>, 4> targets{{
            {'K', "device_kitchen_light"}, {'B', "device_bedroom_light"},
            {'G', "device_living_light"}, {'C', "device_corridor_light"}}};
        std::vector<std::string> canonicalWires;
        for (const auto& [code, target] : targets) {
            for (std::int64_t value = 0; value <= 100; ++value) {
                const auto wire = std::string("L|") + code + "|" + std::to_string(value);
                roundTrip(adapter, light(target, value), wire);
                canonicalWires.push_back(wire);
                const auto zeroPadded = std::string("L|") + code + "|000" + std::to_string(value);
                const auto padded = decodedAction(adapter.decode(zeroPadded));
                check(padded == light(target, value), "frozen leading-zero acceptance without storing raw wire");
                encodedAs(adapter, padded, wire);
                check(toCompact(*ActionValidator::validate(zeroPadded)) == wire, "leading-zero oracle agreement");
            }
        }
        const GenericAction door("action_open_entrance", "device_entrance_door", {});
        roundTrip(adapter, door, "D");
        canonicalWires.push_back("D");
        check(roundTrips == 405, "exactly 404 light + one door round-trips in both directions");

        const auto unsupported = adapter.decode("U");
        check(std::holds_alternative<CandidateResult>(unsupported), "explicit U is a candidate outcome");
        check(std::holds_alternative<Unsupported>(std::get<CandidateResult>(unsupported)), "U never becomes GenericAction");
        check(!ActionValidator::validate("U"), "legacy U also has no Action");

        const std::vector<std::string> badWires{
            "", "L", "L|K", "L|K|", "L|X|50", "L|K|-1", "L|K|101", "L|K|abc", "D|anything",
            "d", "u", "l|K|50", "L|k|50", "L|K|+1", "L|K|1.0", "L|K|1,0", "L|K|1e1",
            "L|K|0x10", "L|K| 50", "L|K|5 0", "L||50", "L|KK|50", "L|K|50|", "L|K|50\nD",
            "L|K|５０", "L|К|50", "L|K|50%", "{\"action\":\"D\"}", "U|", "D U", "D\nU",
            "L|K|9223372036854775807", "L|K|-9223372036854775808", "L|K|18446744073709551615",
            std::string("D\0", 2), std::string("\0D", 2), std::string("L|K|5\0" "0", 7), "\xef\xbb\xbf" "D"};
        for (const auto& wire : badWires) invalidWire(adapter, wire);
        // Exhaustive placement of every normalized-away ASCII whitespace byte:
        // this codec deliberately performs NONE of CommandEngine's normalization.
        for (const auto& wire : canonicalWires) {
            for (const char space : std::string(" \t\r\n\f\v")) {
                for (std::size_t position = 0; position <= wire.size(); ++position) {
                    auto changed = wire;
                    changed.insert(position, 1, space);
                    invalidWire(adapter, changed);
                }
            }
            invalidWire(adapter, wire + "|extra");
            invalidWire(adapter, wire + std::string(1, '\0'));
        }
        for (const char space : std::string(" \t\r\n\f\v")) {
            invalidWire(adapter, std::string(1, space) + "U");
            invalidWire(adapter, "U" + std::string(1, space));
        }
        for (int byte = 0; byte < 256; ++byte) {
            const auto wire = std::string("L|") + static_cast<char>(byte) + "|50";
            const auto found = std::find_if(targets.begin(), targets.end(),
                [byte](const auto& target) { return target.first == byte; });
            if (found == targets.end()) invalidWire(adapter, wire);
            else check(decodedAction(adapter.decode(wire)) == light(found->second, 50), "all valid code bytes mapped by identity");
        }
        for (const auto& [code, target] : targets) {
            for (std::int64_t value = -101; value <= 201; ++value) {
                const auto wire = std::string("L|") + code + "|" + std::to_string(value);
                if (value < 0 || value > 100) {
                    invalidWire(adapter, wire);
                    encodeError(adapter, light(target, value), CompactV1Error::InvalidArguments);
                } else check(decodedAction(adapter.decode(wire)) == light(target, value), "numeric boundary oracle");
            }
            encodeError(adapter, light(target, std::numeric_limits<std::int64_t>::min()), CompactV1Error::InvalidArguments);
            encodeError(adapter, light(target, std::numeric_limits<std::int64_t>::max()), CompactV1Error::InvalidArguments);
        }

        for (const std::string target : {"", "K", "D", "room_kitchen", "Кухня", "device_unknown"})
            encodeError(adapter, light(target, 40), CompactV1Error::UnknownTarget);
        for (const std::string action : {"", "D", "U", "set_brightness", "light.set_brightness.v1", "action_unknown"})
            encodeError(adapter, light("device_kitchen_light", 40, action), CompactV1Error::UnknownAction);
        encodeError(adapter, light("device_entrance_door", 40), CompactV1Error::Unrepresentable);
        encodeError(adapter, GenericAction("action_open_entrance", "device_kitchen_light", {}), CompactV1Error::Unrepresentable);
        encodeError(adapter, GenericAction("action_set_brightness", "device_kitchen_light", {}), CompactV1Error::InvalidArguments);
        encodeError(adapter, GenericAction("action_set_brightness", "device_kitchen_light",
                    {{"level", IntegerArgument(std::int64_t{40})}}), CompactV1Error::InvalidArguments);
        encodeError(adapter, GenericAction("action_set_brightness", "device_kitchen_light",
                    {{"brightness", IntegerArgument(std::int64_t{40})}, {"extra", IntegerArgument(std::int64_t{1})}}),
                    CompactV1Error::InvalidArguments);
        encodeError(adapter, light("device_entrance_door", 40, "action_open_entrance"), CompactV1Error::InvalidArguments);

        bool nullRejected = false;
        try { const CompactV1Adapter missing(nullptr); }
        catch (const std::invalid_argument&) { nullRejected = true; }
        check(nullRejected, "no implicit default snapshot or legacy fallback");

        // Schema-v1 ID mapping is intentionally pinned. Metadata/reordering is
        // valid, but IDs and semantic equality must never depend on display text.
        auto changed = snapshot->configuration();
        for (auto& device : changed.devices) device.name = "Изменённое имя " + device.id;
        for (auto& action : changed.actions) action.name = "Изменённое действие " + action.id;
        std::reverse(changed.devices.begin(), changed.devices.end());
        std::reverse(changed.actions.begin(), changed.actions.end());
        const auto renamed = config::ConfigValidator::validate(changed);
        const CompactV1Adapter renamedAdapter(renamed);
        check(renamedAdapter.snapshot()->contentHash() != adapter.snapshot()->contentHash(), "distinct validated context was actually supplied");
        for (const auto& wire : canonicalWires)
            check(decodedAction(renamedAdapter.decode(wire)) == decodedAction(adapter.decode(wire)), "all stable identities survive metadata/reordering");

        // Add existing supported kinds/operations with new stable IDs in an
        // isolated storage-valid fixture. They are NOT new production features.
        auto extra = snapshot->configuration();
        auto extraLight = extra.devices.at(snapshot->deviceIndex().at("device_kitchen_light"));
        extraLight.id = "device_test_light";
        extra.devices.push_back(extraLight);
        auto extraDoor = extra.devices.at(snapshot->deviceIndex().at("device_entrance_door"));
        extraDoor.id = "device_test_door";
        extra.devices.push_back(extraDoor);
        auto extraAction = extra.actions.at(snapshot->actionIndex().at("action_set_brightness"));
        extraAction.id = "action_test_brightness";
        extra.actions.push_back(extraAction);
        auto otherLight = extraLight;
        otherLight.id = "device_other_test_light";
        otherLight.actionIds = {extraAction.id};
        extra.devices.push_back(otherLight);
        const auto extraSnapshot = config::ConfigValidator::validate(extra);
        const CompactV1Adapter extraAdapter(extraSnapshot);
        check(!extraSnapshot->matchesCurrentLegacyDomain(), "storage-valid extra identities are not legacy-addressable");
        encodeError(adapter, light(extraLight.id, 40), CompactV1Error::UnknownTarget);
        encodeError(extraAdapter, light(extraLight.id, 40), CompactV1Error::Unrepresentable);
        const GenericAction extraDoorAction("action_open_entrance", extraDoor.id, {});
        encodeError(adapter, extraDoorAction, CompactV1Error::UnknownTarget);
        encodeError(extraAdapter, extraDoorAction, CompactV1Error::Unrepresentable);
        const auto extraGeneric = light(otherLight.id, 40, extraAction.id);
        check(extraGeneric.actionId() == extraAction.id && extraGeneric.targetId() == otherLight.id,
              "GenericAction stores actual configured IDs outside the four-code envelope");
        encodeError(adapter, extraGeneric, CompactV1Error::UnknownAction);
        encodeError(extraAdapter, extraGeneric, CompactV1Error::Unrepresentable);
        encodeError(extraAdapter, GenericAction(extraAction.id, otherLight.id, {}), CompactV1Error::InvalidArguments);
        for (const auto& wire : canonicalWires)
            check(decodedAction(extraAdapter.decode(wire)) == decodedAction(adapter.decode(wire)), "codec selects configured mapping, never first device of kind/room");

        changed = snapshot->configuration();
        changed.compatibility.doorTargetId = "missing_door";
        invalidConfiguration(changed);
        changed = snapshot->configuration();
        changed.compatibility.lightTargets['K'] = "missing_light";
        invalidConfiguration(changed);
        changed = extra;
        changed.compatibility.doorTargetId = extraDoor.id;
        invalidConfiguration(changed);
        changed = extra;
        changed.compatibility.lightTargets['K'] = extraLight.id;
        invalidConfiguration(changed);
        changed = extra;
        changed.compatibility.lightActionId = extraAction.id;
        invalidConfiguration(changed);

        // Encoding is not installation support or permission. Narrowing bounds
        // does not redefine the intrinsic compact envelope or erase candidates.
        changed = snapshot->configuration();
        for (auto& action : changed.actions) {
            if (!action.parameters.empty()) {
                action.parameters[0].minimum = 10;
                action.parameters[0].maximum = 90;
            }
            action.enabled = false;
        }
        for (auto& device : changed.devices) device.enabled = false;
        config::DenyAction deny;
        deny.id = "deny_test_door"; deny.name = "Тестовое ограничение";
        deny.deviceId = "device_entrance_door"; deny.actionId = "action_open_entrance";
        changed.restrictions.push_back(deny);
        const CompactV1Adapter narrow(config::ConfigValidator::validate(changed));
        for (const auto& effective : narrow.snapshot()->effectiveActions()) {
            check(!effective.permitted, "fixture has genuinely disabled/denied metadata");
            if (effective.brightnessBounds)
                check(*effective.brightnessBounds == std::make_pair(std::int64_t{10}, std::int64_t{90}), "fixture has actual narrowed bounds");
        }
        for (const auto& wire : canonicalWires) {
            const auto original = decodedAction(adapter.decode(wire));
            check(decodedAction(narrow.decode(wire)) == original, "codec and supported-domain decisions remain separate");
            encodedAs(narrow, original, wire);
        }
        for (const auto& [code, target] : targets) {
            (void)code;
            for (const std::int64_t value : {9, 10, 40, 90, 91})
                check(decodedAction(narrow.decode(std::get<std::string>(narrow.encode(light(target, value))))) == light(target, value),
                      "narrowed bounds neither clamp nor alter candidate representation");
        }

        // Retention is explicit in the adapter; Action values own no snapshot.
        GenericAction retained = door;
        std::weak_ptr<const config::ValidatedCatalog> weak;
        {
            auto temporary = config::ConfigValidator::validate(extra);
            weak = temporary;
            const CompactV1Adapter owner(temporary);
            temporary.reset();
            check(!weak.expired(), "adapter keeps the immutable mapping alive");
            retained = decodedAction(owner.decode("L|K|40"));
        }
        check(weak.expired(), "GenericAction does not retain/own configuration");
        check(retained == light("device_kitchen_light", 40), "candidate owns values after context destruction");
        check(snapshot->canonicalJson() == canonical, "no fixture mutation leaked into original snapshot");

        // Actual gate, not a duplicate safety implementation. An encodable
        // semantic candidate alone is explicitly insufficient for execution.
        encodedAs(adapter, door, "D");
        check(SafetyGate::apply("дверь", "D") == "U", "door safety is separate from codec success");
        check(SafetyGate::apply("открой входную дверь", "D") == "D", "existing valid door control");
        for (const std::string wire : {"U", "L|K|50", "L|K|0"})
            check(SafetyGate::apply("На кухне выключи свет на 50 процентов", wire) == "U", "6.4.1 remains mandatory, independent of adapter");
        check(SafetyGate::apply("На кухне поставь свет на 40 процентов", "L|K|40") == "L|K|40", "existing valid light control");

        std::cout << "CompactV1Adapter: " << checks << " checks passed\n"
                  << "Current-M4 exhaustive round-trips: " << roundTrips << " (404 light + 1 door), both directions\n"
                  << "Malformed wire cases rejected against frozen decoder: " << malformed << '\n'
                  << "Identity/configuration, unsupported arguments, lifetime and safety-separation checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CompactV1Adapter failure: " << error.what() << '\n';
        return 1;
    }
}
