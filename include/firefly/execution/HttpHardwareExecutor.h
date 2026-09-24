#pragma once
#include "firefly/execution/HardwareExecutor.h"

namespace firefly::execution {
// Minimal HTTP transport. No automatic retries — hardware commands may be
// non-idempotent; retry policy belongs on an explicit later binding/action field.
class HttpHardwareExecutor final : public HardwareExecutor {
public:
    HardwareResult execute(const config::Binding& binding,
                           const actions::GenericAction& action) override;
};
}
