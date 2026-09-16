#pragma once
#include "firefly/config/Configuration.h"
#include "firefly/ParserRoomDomain.h"
#include "firefly/LightBrightnessDomain.h"
#include <string_view>

namespace firefly::runtime {
// Read-only installation data, not a command parser or permission to execute.
// Returned pointers remain valid for this view's lifetime. Alias inputs are
// exact canonical keys; request tokenization/normalization is not migrated here.
class DomainView final : public ParserRoomDomain, public LightBrightnessDomain {
public:
    explicit DomainView(config::Catalog snapshot);
    const config::Catalog& snapshot() const { return snapshot_; }
    const config::Room* room(std::string_view id) const;
    const config::Device* device(std::string_view id) const;
    const config::Room* roomMention(std::string_view alias) const;
    const config::Room* roomLocation(std::string_view alias) const;
    std::vector<ParserRoomLocation> parserLocations() const override;
    const config::Device* legacyLightTarget(char code) const;
    const config::Device* entranceTarget() const;
    const config::EffectiveAction* effectiveAction(std::string_view deviceId,
                                                  std::string_view actionId) const;
    std::optional<std::pair<std::int64_t, std::int64_t>> brightnessBounds(std::string_view deviceId) const;
    bool allowsBrightness(std::string_view deviceId, std::int64_t value) const;
    // Production range-only view. Enablement/deny flags are still observational;
    // the permission-aware brightnessBounds/allowsBrightness above are unchanged.
    std::optional<std::pair<std::int64_t, std::int64_t>> lightBrightnessBounds(char roomCode) const;
    bool supportsBrightness(char roomCode, std::int64_t value) const override;
    bool entranceAvailable() const;
private:
    const config::Catalog snapshot_;
};
}
