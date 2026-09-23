#pragma once
#include "firefly/runtime/DomainView.h"
#include <mutex>
#include <variant>

namespace firefly::execution {
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
// One registry belongs to one immutable domain generation. It never queries a
// global/current generation. Request ownership determines the execution domain.
class MockDeviceExecutor final : public ActionExecutor {
public:
    explicit MockDeviceExecutor(config::Catalog catalog);
    ExecutionResult execute(const actions::GenericAction& action) override;
    ExecutionResult preview(const actions::GenericAction& action) const;
    std::vector<DeviceRecord> states() const;
    // Called only before publication. Reject incompatible retained state rather
    // than silently changing it. Removed targets are intentionally not imported.
    void reconcile(const std::vector<DeviceRecord>& previous);
private:
    ExecutionResult evaluate(const actions::GenericAction& action) const;
    runtime::DomainView domain_;
    mutable std::mutex mutex_;
    std::map<std::string, DeviceRecord> devices_;
};
}
