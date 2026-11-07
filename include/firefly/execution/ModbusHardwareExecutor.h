#pragma once
#include "firefly/execution/HardwareExecutor.h"

namespace firefly::execution {
// One explicit request per command/read. No retries, replay, background polling
// or I/O on construction. A write echo establishes acceptance, never state.
class ModbusHardwareExecutor final : public HardwareExecutor {
public:
    explicit ModbusHardwareExecutor(config::Catalog catalog) : catalog_(std::move(catalog)) {}
    HardwareResult execute(const config::Binding&, const actions::GenericAction&) override;
    HardwareObservation observe(const config::ObservationBinding&) override;
    HardwareObservation observe(const config::ObservationBinding&, std::int64_t timeoutLimitMs);
private:
    config::Catalog catalog_;
};
}
