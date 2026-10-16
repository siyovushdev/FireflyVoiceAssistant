#pragma once
#include "firefly/execution/HardwareExecutor.h"
#include <memory>

namespace firefly::execution {
class DeviceStateStore;

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
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
