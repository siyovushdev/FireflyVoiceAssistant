#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include "firefly/actions/GenericAction.h"

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
// Optional preferences preserve canonical bytes of older configurations.
struct VoiceDialogueSettings {
    bool present = false;
    std::string responses = "off"; // off / brief / detailed
    bool questionsEnabled = false;
};
struct VoiceInputSettings {
    bool present = false;
    bool inlineCommands = false;
};
struct System : Identity {
    std::string locale, interpretationProfile;
    VoiceDialogueSettings voiceDialogue;
    VoiceInputSettings voiceInput;
};
// Location is universal; Room / rooms / roomId remain v1 wire/source compatibility names.
struct EntityMetadata {
    std::string type, description, origin;
    bool confirmed = false;
    std::vector<std::string> aliases;
    std::optional<std::string> parentId;
    bool container = false;
    bool present = false;
};
struct Location : Identity {
    std::vector<std::string> mentions, locations;
    EntityMetadata metadata;
};
using Room = Location;
enum class DeviceKind { Unknown, Light, EntranceDoor, Generic };
enum class Operation { Unknown, SetBrightness, OpenEntranceDoor, AssignState };
// v1 keeps its required integer percentage; v2 admits explicit scalar contracts.
struct Parameter : Identity {
    std::int64_t minimum = 0, maximum = 100;
    std::string type = "integer", unit = "percent", description;
    bool required = true;
    double numberMinimum = 0, numberMaximum = 100;
    std::size_t minLength = 0, maxLength = 128;
    std::vector<std::string> values;
};
struct StateField : Parameter { actions::Scalar initial{std::int64_t{0}}; };
struct Assignment {
    std::string field;
    std::optional<std::string> argument;
    std::optional<actions::Scalar> value;
};
struct ActionDefinition : Identity {
    Operation operation = Operation::Unknown;
    bool enabled = false;
    std::vector<Parameter> parameters;
    std::string description, origin;
    std::vector<std::string> aliases;
    std::vector<Assignment> assignments;
};
struct Device : Identity {
    EntityMetadata metadata;
    DeviceKind kind = DeviceKind::Unknown;
    std::optional<std::string> roomId;
    bool enabled = false;
    std::vector<std::string> actionIds;
    std::vector<std::string> spokenAliases;
    std::vector<StateField> state;
};
struct DenyAction : Identity { std::string deviceId, actionId; };
struct LegacyMapping {
    std::string lightActionId, doorActionId, doorTargetId;
    std::map<char, std::string> lightTargets;
};
// Physical transport mapping only. Semantic GenericAction / SafetyGate never read this.
enum class ExecutorKind { Unknown, Http, Mqtt, Tcp, Udp, Serial, Can };
struct HttpConnection {
    std::string baseUrl, basePath;
};
// Network endpoints are shared by MQTT and future TCP/UDP executors.
struct MqttConnectionSettings {
    // Historical disabled MQTT connections had no mqtt object. Preserve their
    // canonical bytes and persisted ConfigStore hash until explicitly edited.
    bool present = false;
    bool tls = false;
    // Identifier only. The actual username/password must never enter ConfigStore.
    std::string credentialsId;
};
struct MqttBinding {
    std::string topic, payload;
    std::int64_t qos = 0;
    bool retain = false;
};
struct MqttObservation {
    std::string topic;
    std::string format = "text"; // text boolean ON/OFF or top-level JSON object
    std::string field;          // target boolean field for text
    std::string truePayload = "ON", falsePayload = "OFF";
    std::int64_t qos = 0;
};
struct NetworkConnection { std::string host; std::int64_t port = 0; };
struct SerialConnection { std::string port; std::int64_t baudRate = 9600; };
struct CanConnection { std::string interface; };
struct Connection : Identity {
    ExecutorKind type = ExecutorKind::Unknown;
    bool enabled = true;
    std::int64_t timeoutMs = 2000;
    HttpConnection http;
    MqttConnectionSettings mqtt;
    NetworkConnection network;
    SerialConnection serial;
    CanConnection can;
};
struct HttpBinding {
    std::string method = "POST";
    std::string url;
    std::int64_t timeoutMs = 2000;
    std::string contentType = "application/json";
    // Fixed JSON object fields; values must be JSON scalars.
    std::map<std::string, actions::Scalar> body;
    // Optional validated GenericAction argument id copied into the request body.
    std::optional<std::string> argument;
    std::string path;
    // 0 = any 2xx for legacy mappings; new mappings explicitly select a status.
    std::int64_t expectedStatus = 0;
    std::map<std::string, std::string> headers;
};
struct Binding {
    std::string targetId, actionId;
    ExecutorKind executor = ExecutorKind::Unknown;
    HttpBinding http;
    MqttBinding mqtt;
    std::string connectionId;
};
struct ObservationBinding {
    std::string targetId, connectionId;
    ExecutorKind executor = ExecutorKind::Http;
    HttpBinding http;
    MqttObservation mqtt;
    // Top-level JSON response member -> existing state property. No expressions.
    std::map<std::string, std::string> fields;
};
struct Configuration {
    std::int64_t schemaVersion = 1, revision = 1;
    std::vector<std::string> retiredIds;
    System system;
    std::vector<Room> rooms;
    std::vector<Device> devices;
    std::vector<ActionDefinition> actions;
    std::vector<DenyAction> restrictions;
    std::vector<Binding> bindings;
    std::vector<Connection> connections;
    std::vector<ObservationBinding> observations;
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
    // Exactly one binding per (targetId, actionId) after validation.
    const std::map<std::pair<std::string, std::string>, std::size_t>& bindingIndex() const { return bindings_; }
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
    std::map<std::pair<std::string, std::string>, std::size_t> bindings_;
    std::vector<EffectiveAction> effective_;
    std::vector<std::string> legacyDifferences_;
    std::string canonical_, hash_;
};
using Catalog = std::shared_ptr<const ValidatedCatalog>;
bool accepts(const Parameter& schema, const actions::Scalar& value);
// Explicit v1 -> v2 candidate migration. Does not write a store or change IDs.
Configuration migrateV2(Configuration value);
}
