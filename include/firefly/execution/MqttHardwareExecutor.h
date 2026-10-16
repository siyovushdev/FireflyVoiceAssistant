#pragma once
#include "firefly/execution/HardwareExecutor.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>

namespace firefly::execution {
class DeviceStateStore;

// A broker message begun before/during a command cannot be used as a fresh
// confirmation after the command. The gate is independent of wall-clock ms.
class MqttObservationGate final {
public:
    std::uint64_t snapshot() const;
    void begin(const std::string& target);
    void end(const std::string& target);
    bool accept(const std::string& target, std::uint64_t snapshot,
                const std::function<void()>& commit);
private:
    mutable std::mutex mutex_;
    std::atomic<std::uint64_t> epoch_{0};
    std::map<std::string, std::size_t> pending_;
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
