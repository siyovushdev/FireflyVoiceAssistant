#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace firefly::config {
inline constexpr std::size_t MaxConfigBytes = 256 * 1024;
inline constexpr std::int64_t MaxRevision = 9007199254740991LL;

class ConfigError : public std::runtime_error {
public:
    ConfigError(std::string code, std::string path, std::string message);
    const std::string code;
    const std::string path;
};

struct Identity { std::string id, name; };
struct System : Identity { std::string locale, interpretationProfile; };
struct Room : Identity { std::vector<std::string> mentions, locations; };
enum class DeviceKind { Unknown, Light, EntranceDoor };
enum class Operation { Unknown, SetBrightness, OpenEntranceDoor };
// The only v1 argument contract is a required integer percentage. Other
// parameter types/units cannot be represented or injected into this snapshot.
struct Parameter : Identity {
    std::int64_t minimum = 0, maximum = 100;
    static constexpr const char* type = "integer";
    static constexpr const char* unit = "percent";
    static constexpr bool required = true;
};
struct ActionDefinition : Identity {
    Operation operation = Operation::Unknown;
    bool enabled = false;
    std::vector<Parameter> parameters;
};
struct Device : Identity {
    DeviceKind kind = DeviceKind::Unknown;
    std::optional<std::string> roomId;
    bool enabled = false;
    std::vector<std::string> actionIds;
};
struct DenyAction : Identity { std::string deviceId, actionId; };
struct LegacyMapping {
    std::string lightActionId, doorActionId, doorTargetId;
    std::map<char, std::string> lightTargets;
};
struct Configuration {
    std::int64_t schemaVersion = 1, revision = 1;
    std::vector<std::string> retiredIds;
    System system;
    std::vector<Room> rooms;
    std::vector<Device> devices;
    std::vector<ActionDefinition> actions;
    std::vector<DenyAction> restrictions;
    LegacyMapping compatibility;
};
struct EffectiveAction {
    std::string deviceId, actionId;
    Operation operation = Operation::Unknown;
    bool permitted = false;
    std::optional<std::pair<std::int64_t, std::int64_t>> brightnessBounds;
    std::vector<std::string> mandatoryPolicies;
};

// No mutable alias is retained. All accessors are const; construction is private.
class ValidatedCatalog final {
public:
    const Configuration& configuration() const { return configuration_; }
    const std::map<std::string, std::size_t>& roomIndex() const { return rooms_; }
    const std::map<std::string, std::size_t>& deviceIndex() const { return devices_; }
    const std::map<std::string, std::size_t>& actionIndex() const { return actions_; }
    const std::map<std::string, std::string>& mentionIndex() const { return mentions_; }
    const std::map<std::string, std::string>& locationIndex() const { return locations_; }
    const std::vector<EffectiveAction>& effectiveActions() const { return effective_; }
    const std::vector<std::string>& legacyDifferences() const { return legacyDifferences_; }
    bool matchesCurrentLegacyDomain() const { return legacyDifferences_.empty(); }
    const std::string& canonicalJson() const { return canonical_; }
    const std::string& contentHash() const { return hash_; }
    std::string summary() const;
private:
    friend class ConfigValidator;
    ValidatedCatalog() = default;
    Configuration configuration_;
    std::map<std::string, std::size_t> rooms_, devices_, actions_;
    std::map<std::string, std::string> mentions_, locations_;
    std::vector<EffectiveAction> effective_;
    std::vector<std::string> legacyDifferences_;
    std::string canonical_, hash_;
};
using Catalog = std::shared_ptr<const ValidatedCatalog>;
}
