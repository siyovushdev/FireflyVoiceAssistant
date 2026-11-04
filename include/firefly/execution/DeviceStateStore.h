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
using WallClockMs = std::function<std::int64_t()>;

struct PropertyKnowledge {
    std::optional<actions::Scalar> value;
    StateKnowledge knowledge = StateKnowledge::Unknown;
    std::int64_t updatedAtMs = 0;
    std::int64_t observedAtMs = 0;
    // Process-local only: never serialized as epoch/display timestamps.
    std::optional<std::int64_t> freshnessSteadyMs;
    std::optional<std::int64_t> orderSteadyMs;
    // Historical observation, independent of a later command prediction.
    std::optional<actions::Scalar> lastObservedValue;
    std::optional<std::int64_t> lastObservationSteadyMs;
};

struct DeviceKnowledge {
    std::string targetId;
    std::string name;
    std::map<std::string, PropertyKnowledge> properties;
    Connectivity connectivity = Connectivity::Unknown;
    std::int64_t lastSuccessAtMs = 0;
    std::int64_t lastFailureAtMs = 0;
    std::int64_t lastObservationAtMs = 0;
    std::optional<std::int64_t> lastObservationSteadyMs;
    bool observationDisconnected = false;
};

// Independent observation/readback. Future HTTP/MQTT/GPIO transports feed this
// shape without touching CommandEngine / SafetyGate / GenericAction.
struct StateObservation {
    std::string targetId;
    std::map<std::string, actions::Scalar> properties;
    // Unix/event display time, NOT age or ordering. Zero samples the local wall clock.
    std::int64_t timestampMs = 0;
    // Capture from this process's steady clock when a local read is initiated.
    // Explicit event timestamps require this stamp; a source timestamp alone
    // cannot establish local order. Null + timestampMs=0 is synchronous arrival.
    std::optional<std::int64_t> orderSteadyMs;
};

inline bool isStale(const PropertyKnowledge& property, std::int64_t steadyNowMs,
                    std::int64_t ttlMs = kDefaultStateTtlMs) {
    if (property.knowledge == StateKnowledge::Unknown || ttlMs <= 0 || !property.freshnessSteadyMs) return false;
    return steadyNowMs - *property.freshnessSteadyMs > ttlMs;
}

// Device-origin observations only; a command ACK or broker connection cannot
// establish availability. Offline means no telemetry within the 60s budget,
// or a lost observation connection, not proof the device is powered off.
inline std::optional<std::int64_t> observationAge(const DeviceKnowledge& device, std::int64_t now) {
    if (!device.lastObservationSteadyMs || *device.lastObservationSteadyMs > now) return std::nullopt;
    return now - *device.lastObservationSteadyMs;
}
inline const char* availability(const DeviceKnowledge& device, std::int64_t now) {
    if (device.observationDisconnected) return "offline";
    const auto age = observationAge(device, now);
    return !age ? "unknown" : *age > kDefaultStateTtlMs ? "offline" : "online";
}

// Authoritative production physical knowledge. Not owned by HardwareExecutor.
// Cold start / process restart → all Unknown (no durable telemetry in M15).
// Config-generation reconcile may retain in-process Predicted/Confirmed values
// that remain schema-compatible; restart is a separate event.
class DeviceStateStore final {
public:
    explicit DeviceStateStore(config::Catalog catalog, SteadyClockMs steadyClock = {}, WallClockMs wallClock = {});

    std::vector<DeviceKnowledge> devices() const;
    std::optional<DeviceKnowledge> device(const std::string& targetId) const;

    // After Accepted hardware execution: expected property map as Predicted.
    void applyPrediction(const std::string& targetId,
                         const std::map<std::string, actions::Scalar>& expected,
                         std::int64_t atMs, std::optional<std::int64_t> startedSteadyMs = std::nullopt);
    // Independent telemetry/readback — Confirmed; overrides Predicted when the
    // local steady order stamp is not older than existing property knowledge.
    // Older observations are rejected (stale_observation). Values must obey
    // built-in / Generic schema invariants (unsafe_constraint).
    void applyObservation(const StateObservation& observation);
    // Dry-run schema/order validation before buffering an MQTT state sample.
    void validateObservation(const StateObservation& observation) const;
    void markReachable(const std::string& targetId, std::int64_t atMs);
    void markUnreachable(const std::string& targetId, std::int64_t atMs);
    // Fail closed for execution, but retain explicitly historical observations.
    void observationDisconnected(const std::string& targetId);

    // In-process generation Apply: retain compatible knowledge; new devices Unknown;
    // removed devices drop; incompatible schema → state_conflict.
    void reconcile(const DeviceStateStore& previous);
    // A changed physical mapping cannot inherit evidence from the old endpoint.
    void invalidate(const std::string& targetId);

    std::int64_t nowMs() const; // Wall/Unix time for public event fields only.
    std::int64_t steadyNowMs() const; // Private TTL/local-order domain.
    static std::string knowledgeName(StateKnowledge value);
    static std::string connectivityName(Connectivity value);

private:
    void initializeUnknown(const config::Device& device);
    void validateObservationValue(const config::Device& device, const std::string& key,
                                  const actions::Scalar& value) const;
    void validateObservationLocked(const StateObservation& observation,
                                   const DeviceKnowledge& current, std::int64_t arrival) const;
    runtime::DomainView domain_;
    SteadyClockMs steadyClock_;
    WallClockMs wallClock_;
    mutable std::mutex mutex_;
    std::map<std::string, DeviceKnowledge> devices_;
};
}
