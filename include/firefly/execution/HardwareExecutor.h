#pragma once
#include "firefly/config/Configuration.h"
#include "firefly/actions/GenericAction.h"
#include <map>
#include <memory>
#include <string>

namespace firefly::execution {
// Physical transport outcome only. Semantic SafetyGate / domain rejections never
// become HardwareResult — those stop in CommandEngine before any executor call.
struct HardwareResult {
    bool success = false;
    // Stable machine code (timeout, connection_failure, http_error, ...).
    std::string error;
    // Optional transport diagnostic (e.g. HTTP status digits). Never interpreted
    // by generic BoundDeviceExecutor beyond opaque append for logs/UI.
    std::string detail;
};

class HardwareExecutor {
public:
    virtual ~HardwareExecutor() = default;
    virtual HardwareResult execute(const config::Binding& binding,
                                   const actions::GenericAction& action) = 0;
};

class ExecutorRegistry {
public:
    ExecutorRegistry();
    void registerExecutor(config::ExecutorKind kind, std::shared_ptr<HardwareExecutor> executor);
    HardwareResult execute(const config::Binding& binding, const actions::GenericAction& action) const;
private:
    std::map<config::ExecutorKind, std::shared_ptr<HardwareExecutor>> executors_;
};

class BindingResolver {
public:
    enum class Status { Found, Missing, Ambiguous };
    struct Result {
        Status status = Status::Missing;
        const config::Binding* binding = nullptr;
    };
    explicit BindingResolver(config::Catalog catalog);
    Result resolve(const std::string& targetId, const std::string& actionId) const;
private:
    config::Catalog catalog_;
};
}
