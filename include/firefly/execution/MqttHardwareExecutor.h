#pragma once
#include "firefly/execution/HardwareExecutor.h"
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

namespace firefly::execution {
class DeviceStateStore;

// Per-target command/observation barrier. It does not depend on wall-clock
// resolution or on commands issued to unrelated targets. A state arriving
// after publish is submitted but before PUBACK is buffered until prediction.
class MqttObservationGate final {
public:
    std::uint64_t snapshot(const std::string& target) const;
    void begin(const std::string& target);
    void publicationStarted(const std::string& target);
    void end(const std::string& target, DeviceStateStore& store) noexcept;
    // true: accepted or buffered; false: obsolete or pre-publish telemetry.
    bool accept(const std::string& target, std::uint64_t snapshot,
                const std::map<std::string, actions::Scalar>& properties,
                DeviceStateStore& store);
private:
    struct Target {
        std::uint64_t epoch = 0;
        bool inFlight = false;
        bool published = false;
        std::optional<std::map<std::string, actions::Scalar>> buffered;
    };
    mutable std::mutex mutex_;
    std::map<std::string, Target> targets_;
};

// Strict codec is available in host CPU tests without any network access.
std::map<std::string, actions::Scalar> decodeMqttObservationPayload(
    const config::ObservationBinding& binding, std::string_view payload);

// A single RuntimeGeneration owns MQTT resources. No network I/O during
// construction, draft validation or preflight; subscriptions start after Apply.
class MqttHardwareExecutor final : public HardwareExecutor {
public:
    MqttHardwareExecutor(config::Catalog catalog, DeviceStateStore& state);
    ~MqttHardwareExecutor() override;
    HardwareResult execute(const config::Binding& binding,
                           const actions::GenericAction& action) override;
    HardwareObservation observe(const config::ObservationBinding& binding) override;
    void startSubscriptions();
    void beginCommand(const std::string& target);
    void endCommand(const std::string& target);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
