#pragma once
#include <optional>
#include <string>

namespace firefly {
// Resolved installation identity accompanying a legacy D result, not a new
// Action/protocol or permission to execute. Names never identify a target.
struct EntranceDoorTarget {
    std::string deviceId;
    std::string displayName;
};
class EntranceDoorDomain {
public:
    virtual ~EntranceDoorDomain() = default;
    // Only the configured compact-D target. No scanning for another door or
    // substituting a legacy identity when it cannot be resolved.
    virtual std::optional<EntranceDoorTarget> resolveEntranceDoor() const = 0;
};
}
