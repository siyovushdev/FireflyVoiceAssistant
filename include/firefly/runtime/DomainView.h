#pragma once
#include "firefly/config/Configuration.h"
#include "firefly/ParserRoomDomain.h"
#include "firefly/actions/GenericAction.h"
#include <string_view>
namespace firefly::runtime {
struct ActionDescription {
    const config::Device* target;
    const config::ActionDefinition* definition;
    std::optional<std::int64_t> brightness;
};
// Immutable domain view. Borrowed descriptions live as long as this view.
class DomainView final : public ParserRoomDomain {
public:
    explicit DomainView(config::Catalog snapshot);
    const config::Catalog& snapshot() const { return snapshot_; }
    const config::Room* room(std::string_view id) const;
    const config::Device* device(std::string_view id) const;
    const config::Room* roomMention(std::string_view alias) const;
    const config::Room* roomLocation(std::string_view alias) const;
    std::vector<ParserRoomLocation> parserLocations() const override;
    const config::Device* entranceTarget() const;
    const config::EffectiveAction* effectiveAction(std::string_view deviceId, std::string_view actionId) const;
    // Structural semantics, then installation range/selected target. This type
    // cutover deliberately does not activate enablement/deny metadata.
    std::optional<ActionDescription> describe(const actions::GenericAction& candidate) const;
    actions::CandidateResult validate(const actions::CandidateResult& candidate) const;
private:
    const config::Catalog snapshot_;
};
}
