#pragma once
#include "firefly/llm/StructuredOutput.h"
#include <optional>
#include <utility>
#include <vector>

namespace firefly::llm {
enum class PowerPolarity { Unspecified, On, Off, Contradictory };
PowerPolarity powerPolarity(std::string_view phrase);
bool powerRequestForm(std::string_view phrase);
// A request-scoped choice between two existing boolean power assignments.
// It never contains transport addresses, mutable catalog state or new actions.
struct PowerIntentScope {
    std::string targetId, offActionId, onActionId;
    // Original byte spans authorized by the shared request-form analysis:
    // one bounded intent plus positional courtesy forms, never a rewritten input.
    std::vector<std::pair<std::size_t, std::size_t>> requestSpans;
    std::optional<bool> explicitPower;
    std::string contract(const runtime::DomainView& domain) const;
    DecodedResult decode(std::string_view raw, const runtime::DomainView& domain) const;
};
std::optional<PowerIntentScope> powerIntentScope(const runtime::DomainView& domain,
                                                std::string_view original);
std::string powerIntentRules();
}
