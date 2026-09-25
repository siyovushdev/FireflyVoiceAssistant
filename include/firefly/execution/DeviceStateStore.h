#pragma once
#include "firefly/runtime/DomainView.h"
#include "firefly/actions/GenericAction.h"
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace firefly::execution {
// Three independent axes — never collapse into one enum.
enum class ExecutionStatus { Accepted, Failed };
enum class StateKnowledge { Unknown, Predicted, Confirmed };
enum class Connectivity { Unknown, Reachable, Unreachable };

inline constexpr std::int64_t kDefaultStateTtlMs = 60'000;

using SteadyClockMs = std::function<std::int64_t()>;

struct PropertyKnowledge {
    std::optional<actions::Scalar> value;
    StateKnowledge knowledge = StateKnowledge::Unknown;
    std::int64_t updatedAtMs = 0;
    std::int64_t observedAtMs = 0;
};

struct DeviceKnowledge {
    std::string targetId;
    std::string name;
    std::map<std::string, PropertyKnowledge> properties;
    Connectivity connectivity = Connectivity::Unknown;
    std::int64_t lastSuccessAtMs = 0;
    std::int64_t lastFailureAtMs = 0;
};

// Independent observation/readback. Future HTTP/MQTT/GPIO transports feed this
// shape without touching CommandEngine / SafetyGate / GenericAction.
struct StateObservation {
    std::string targetId;
    std::map<std::string, actions::Scalar> properties;
    std::int64_t timestampMs = 0;
};

inline bool isStale(const PropertyKnowledge& property, std::int64_t nowMs,
                    std::int64_t ttlMs = kDefaultStateTtlMs) {
    if (property.knowledge == StateKnowledge::Unknown || ttlMs <= 0) return false;
    const auto stamp = property.knowledge == StateKnowledge::Confirmed
        ? property.observedAtMs : property.updatedAtMs;
    if (stamp <= 0) return false;
    return nowMs - stamp > ttlMs;
}

// Authoritative production physical knowledge. Not owned by HardwareExecutor.
// Cold start / process restart → all Unknown (no durable telemetry in M15).
// Config-generation reconcile may retain in-process Predicted/Confirmed values
// that remain schema-compatible; restart is a separate event.
class DeviceStateStore final {
public:
    explicit DeviceStateStore(config::Catalog catalog, SteadyClockMs clock = {});

    std::vector<DeviceKnowledge> devices() const;
    std::optional<DeviceKnowledge> device(const std::string& targetId) const;

    // After Accepted hardware execution: expected property map as Predicted.
    void applyPrediction(const std::string& targetId,
                         const std::map<std::string, actions::Scalar>& expected,
                         std::int64_t atMs);
    // Independent telemetry/readback — Confirmed; overrides Predicted when the
    // observation timestamp is not older than existing property knowledge.
    // Older observations are rejected (stale_observation). Values must obey
    // built-in / Generic schema invariants (unsafe_constraint).
    void applyObservation(const StateObservation& observation);
    void markReachable(const std::string& targetId, std::int64_t atMs);
    void markUnreachable(const std::string& targetId, std::int64_t atMs);

    // In-process generation Apply: retain compatible knowledge; new devices Unknown;
    // removed devices drop; incompatible schema → state_conflict.
    void reconcile(const DeviceStateStore& previous);

    std::int64_t nowMs() const;
    static std::string knowledgeName(StateKnowledge value);
    static std::string connectivityName(Connectivity value);

private:
    void initializeUnknown(const config::Device& device);
    void validateObservationValue(const config::Device& device, const std::string& key,
                                  const actions::Scalar& value) const;
    static std::int64_t relevantStamp(const PropertyKnowledge& property);
    runtime::DomainView domain_;
    SteadyClockMs clock_;
    mutable std::mutex mutex_;
    std::map<std::string, DeviceKnowledge> devices_;
};
}
