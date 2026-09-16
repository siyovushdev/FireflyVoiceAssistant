#pragma once
#include <cstdint>

namespace firefly {
// Range-only installation constraint for a decoded compact-v1 light target.
// No parsing, persistence, enablement or safety-policy API. A miss must reject.
class LightBrightnessDomain {
public:
    virtual ~LightBrightnessDomain() = default;
    virtual bool supportsBrightness(char roomCode, std::int64_t value) const = 0;
};
}
