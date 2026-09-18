#include "firefly/runtime/ShadowComparison.h"
#include "firefly/ActionValidator.h"
#include "firefly/DeterministicParser.h"
#include "firefly/SafetyGate.h"

namespace firefly::runtime {
void ShadowComparison::requireEquivalent() const {
    if (!equivalent()) throw std::runtime_error("Configuration shadow mismatch: " + mismatches.front());
}
ShadowComparison compareLegacyEntranceDoor(const DomainView& domain) {
    ShadowComparison result;
    const auto legacy = ActionValidator::validate("D");
    const bool legacyEntrance = legacy && std::holds_alternative<OpenEntranceDoor>(*legacy);
    ++result.checks;
    if (domain.resolveEntranceDoor().has_value() != legacyEntrance)
        result.mismatches.push_back("entrance_target");
    // Frozen D can address one entrance only. Extra stored entrances are
    // observable profile drift, never alternative candidates for resolution.
    std::size_t configuredEntrances = 0;
    for (const auto& device : domain.snapshot()->configuration().devices)
        if (device.kind == config::DeviceKind::EntranceDoor) ++configuredEntrances;
    ++result.checks;
    if (configuredEntrances != static_cast<std::size_t>(legacyEntrance))
        result.mismatches.push_back("entrance_entity_set");
    return result;
}
ShadowComparison compareLegacyBrightness(const DomainView& domain) {
    ShadowComparison result;
    const auto check = [&](bool equal, const std::string& key) {
        ++result.checks;
        if (!equal) result.mismatches.push_back(key);
    };
    for (int byte = 0; byte <= 255; ++byte) {
        const char code = static_cast<char>(static_cast<unsigned char>(byte));
        const std::string prefix = "L|" + std::string(1, code) + "|";
        const auto legacyTarget = ActionValidator::validate(prefix + "50");
        const auto bounds = domain.lightBrightnessBounds(code);
        const std::string key = "code:" + std::to_string(byte);
        check(legacyTarget.has_value() == bounds.has_value(), key);
        if (!legacyTarget && !bounds) continue;
        // Frozen compact percentage envelope plus its adjacent boundaries.
        // These are comparison probes, never production support defaults.
        for (int value = -1; value <= 101; ++value) {
            const bool legacy = ActionValidator::validate(prefix + std::to_string(value)).has_value();
            check(domain.supportsBrightness(code, value) == legacy,
                  key + ":brightness:" + std::to_string(value));
        }
    }
    return result;
}
ShadowComparison compareLegacyDomain(const DomainView& domain) {
    ShadowComparison result;
    const auto check = [&](bool equal, const std::string& key) {
        ++result.checks;
        if (!equal) result.mismatches.push_back(key);
    };
    // Reuse the validated versioned contract to catch missing aliases/entities.
    // Do not create another copy of the legacy installation tables here.
    check(domain.snapshot()->matchesCurrentLegacyDomain(), "legacy_profile");
    for (const auto& difference : domain.snapshot()->legacyDifferences())
        result.mismatches.push_back("profile:" + difference);

    // The complete byte code space finds missing/extra supported legacy rooms.
    // Numeric probes are the frozen protocol domain plus both adjacent boundaries.
    for (int byte = 0; byte <= 255; ++byte) {
        const char code = static_cast<char>(static_cast<unsigned char>(byte));
        const std::string prefix = "L|" + std::string(1, code) + "|";
        const auto oldRoom = ActionValidator::validate(prefix + "50");
        const auto* target = domain.legacyLightTarget(code);
        const std::string key = "code:" + std::to_string(byte);
        check(oldRoom.has_value() == (target != nullptr), key);
        if (!oldRoom && !target) continue;
        for (int value = -1; value <= 101; ++value) {
            const bool oldAllowed = ActionValidator::validate(prefix + std::to_string(value)).has_value();
            const bool shadowAllowed = target && domain.allowsBrightness(target->id, value);
            check(oldAllowed == shadowAllowed, key + ":brightness:" + std::to_string(value));
        }
        for (const auto& [alias, roomId] : domain.snapshot()->mentionIndex()) {
            const auto* resolved = domain.roomMention(alias);
            const bool shadowMention = target && target->roomId && resolved && resolved->id == *target->roomId;
            const bool oldMention = SafetyGate::apply(alias + " свет 40", prefix + "40") == prefix + "40";
            check(oldMention == shadowMention, key + ":mention:" + alias);
            check(resolved && resolved->id == roomId, "mention_index:" + alias);
        }
    }
    for (const auto& [alias, roomId] : domain.snapshot()->locationIndex()) {
        const auto* resolved = domain.roomLocation(alias);
        check(resolved && resolved->id == roomId, "location_index:" + alias);
        for (const auto& [code, targetId] : domain.snapshot()->configuration().compatibility.lightTargets) {
            const auto* target = domain.device(targetId);
            if (!resolved || !target || target->roomId != resolved->id) continue;
            for (int value = 0; value <= 100; ++value) {
                const auto parsed = DeterministicParser::parse(alias + " свет " + std::to_string(value));
                const auto compact = "L|" + std::string(1, code) + "|" + std::to_string(value);
                check((parsed == compact) == domain.allowsBrightness(targetId, value),
                      "location:" + alias + ":brightness:" + std::to_string(value));
            }
        }
    }
    const auto door = ActionValidator::validate("D");
    check((door && std::holds_alternative<OpenEntranceDoor>(*door)) == domain.entranceAvailable(), "entrance_target");
    return result;
}
}
