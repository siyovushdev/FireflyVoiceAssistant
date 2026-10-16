#pragma once
#include "firefly/runtime/DomainView.h"
#include "firefly/execution/HardwareExecutor.h"
#include "firefly/execution/DeviceStateStore.h"
#include <mutex>
#include <variant>

namespace firefly::execution {
// Explicit execution intent. Never inferred from bindings[] or diagnostic source labels.
enum class ExecutionIntent {
    None,        // parse / SafetyGate / validate only
    Playground,  // Admin test-command: MockDeviceExecutor only, never HTTP / production store
    Production   // voice / runtime command: hardware path + DeviceStateStore
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
    ExecutionStatus status = ExecutionStatus::Failed;
    std::string actionId, targetId, error;
    std::optional<DeviceState> previous, current;
    std::int64_t timestampMs = 0;
};
class ActionExecutor {
public:
    virtual ~ActionExecutor() = default;
    virtual ExecutionResult execute(const actions::GenericAction& action) = 0;
};
// Generation-owned playground simulation + domain preview surface.
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
// Generation-owned facade:
//   Playground → MockDeviceExecutor only (no HTTP, no production store/connectivity)
//   Production → BindingResolver → HardwareExecutor → DeviceStateStore Predicted/connectivity
class MqttHardwareExecutor;
class BoundDeviceExecutor final : public DeviceStateExecutor {
public:
    explicit BoundDeviceExecutor(config::Catalog catalog, SteadyClockMs steadyClock = {}, WallClockMs wallClock = {});
    ExecutionResult execute(const actions::GenericAction& action) override;
    ExecutionResult execute(const actions::GenericAction& action, ExecutionIntent intent);
    ExecutionResult preview(const actions::GenericAction& action) const override;
    // Playground virtual states (simulation / Apply preview). Not production knowledge.
    std::vector<DeviceRecord> states() const override;
    void reconcile(const std::vector<DeviceRecord>& previous) override;
    void reconcile(const BoundDeviceExecutor& previous);
    DeviceStateStore& production();
    const DeviceStateStore& production() const;
    void observe(const StateObservation& observation);
    HardwareResult poll(const std::string& targetId);
    // Invoked only after the generation is published, never during preflight.
    void startSubscriptions();
private:
    config::Catalog catalog_;
    MockDeviceExecutor playground_;
    DeviceStateStore production_;
    BindingResolver resolver_;
    ExecutorRegistry registry_;
    std::shared_ptr<MqttHardwareExecutor> mqtt_;
};
}
