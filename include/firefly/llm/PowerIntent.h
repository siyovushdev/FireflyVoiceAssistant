#pragma once
#include "firefly/llm/StructuredOutput.h"
#include <optional>

namespace firefly::llm {
enum class PowerPolarity { Unspecified, On, Off, Contradictory };
PowerPolarity powerPolarity(std::string_view phrase);
bool powerRequestForm(std::string_view phrase);
// A request-scoped choice between two existing boolean power assignments.
// It never contains transport addresses, mutable catalog state or new actions.
struct PowerIntentScope {
    std::string targetId, offActionId, onActionId;
    std::size_t intentBegin = 0, intentEnd = 0;
    std::optional<bool> explicitPower;
    std::string contract(const runtime::DomainView& domain) const;
    DecodedResult decode(std::string_view raw, const runtime::DomainView& domain) const;
};
std::optional<PowerIntentScope> powerIntentScope(const runtime::DomainView& domain,
                                                std::string_view original);
std::string powerIntentRules();
}
