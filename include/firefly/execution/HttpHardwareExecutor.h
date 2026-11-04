#pragma once
#include "firefly/execution/HardwareExecutor.h"

namespace firefly::execution {
// Minimal HTTP transport. No automatic retries — hardware commands may be
// non-idempotent; retry policy belongs on an explicit later binding/action field.
class HttpHardwareExecutor final : public HardwareExecutor {
public:
    explicit HttpHardwareExecutor(config::Catalog catalog = {}) : catalog_(std::move(catalog)) {}
    HardwareResult execute(const config::Binding& binding,
                           const actions::GenericAction& action) override;
    HardwareObservation observe(const config::ObservationBinding& binding) override;
    // Optional stricter deadline for a bounded voice read; never extends the configured timeout.
    HardwareObservation observe(const config::ObservationBinding& binding, std::int64_t timeoutLimitMs);
    // Connect-only reachability probe: sends zero HTTP bytes.
    static HardwareResult testConnection(const config::Connection& connection);
private:
    config::Catalog catalog_;
    HardwareResult prepare(const std::string& connectionId, const config::HttpBinding& mapping,
                           config::HttpBinding& effective, bool observe = false) const;
};
}
