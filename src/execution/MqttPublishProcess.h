#pragma once
#include "firefly/execution/MqttHardwareExecutor.h"
#include <chrono>
namespace firefly::execution::detail {
HardwareResult supervisedMqttPublish(const config::Connection& connection,
    const config::MqttBinding& mapping, MqttObservationGate& gate,
    const std::string& target, std::chrono::steady_clock::time_point deadline);
}
