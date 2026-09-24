#pragma once
#include "firefly/runtime/DomainView.h"
#include "firefly/execution/HardwareExecutor.h"
#include <mutex>
#include <variant>

namespace firefly::execution {
// Explicit execution intent. Never inferred from bindings[] or diagnostic source labels.
enum class ExecutionIntent {
    None,        // parse / SafetyGate / validate only
    Playground,  // Admin test-command: MockDeviceExecutor only, never HTTP
    Production   // voice / runtime command: BoundDeviceExecutor hardware path
};
struct LightState { std::int64_t brightness; };
enum class DoorPosition { Closed, Open };
struct DoorState { DoorPosition position = DoorPosition::Closed; };
struct InventoryState {};
using TypedState = std::map<std::string, actions::Scalar>;
inline bool operator==(InventoryState, InventoryState) { return true; }
using DeviceState = std::variant<LightState, DoorState, InventoryState, TypedState>;
inline bool operator==(LightState a, LightState b) { return a.brightness == b.brightness; }
inline bool operator==(DoorState a, DoorState b) { return a.position == b.position; }
struct DeviceRecord { std::string targetId, name; DeviceState state; };
struct ExecutionResult {
    bool success = false;
    std::string actionId, targetId, error;
    std::optional<DeviceState> previous, current;
};
class ActionExecutor {
public:
    virtual ~ActionExecutor() = default;
    virtual ExecutionResult execute(const actions::GenericAction& action) = 0;
};
// Generation-owned state + execute surface shared by offline mock and hardware paths.
class DeviceStateExecutor : public ActionExecutor {
public:
    virtual ExecutionResult preview(const actions::GenericAction& action) const = 0;
    virtual std::vector<DeviceRecord> states() const = 0;
    virtual void reconcile(const std::vector<DeviceRecord>& previous) = 0;
};
// One registry belongs to one immutable domain generation. It never queries a
// global/current generation. Request ownership determines the execution domain.
class MockDeviceExecutor final : public DeviceStateExecutor {
public:
    explicit MockDeviceExecutor(config::Catalog catalog);
    ExecutionResult execute(const actions::GenericAction& action) override;
    ExecutionResult preview(const actions::GenericAction& action) const override;
    std::vector<DeviceRecord> states() const override;
    // Called only before publication. Reject incompatible retained state rather
    // than silently changing it. Removed targets are intentionally not imported.
    void reconcile(const std::vector<DeviceRecord>& previous) override;
private:
    ExecutionResult evaluate(const actions::GenericAction& action) const;
    runtime::DomainView domain_;
    mutable std::mutex mutex_;
    std::map<std::string, DeviceRecord> devices_;
};
// Generation-owned production executor. Local mock state is shared for observe /
// reconcile / playground virtual execute. SafetyGate runs in CommandEngine only.
// Playground intent never reaches BindingResolver or HardwareExecutor.
class BoundDeviceExecutor final : public DeviceStateExecutor {
public:
    explicit BoundDeviceExecutor(config::Catalog catalog);
    // ActionExecutor default is Production (fail-closed hardware path).
    ExecutionResult execute(const actions::GenericAction& action) override;
    ExecutionResult execute(const actions::GenericAction& action, ExecutionIntent intent);
    ExecutionResult preview(const actions::GenericAction& action) const override;
    std::vector<DeviceRecord> states() const override;
    void reconcile(const std::vector<DeviceRecord>& previous) override;
private:
    MockDeviceExecutor mock_;
    BindingResolver resolver_;
    ExecutorRegistry registry_;
};
}
