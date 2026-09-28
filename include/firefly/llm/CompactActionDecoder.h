#pragma once
#include "firefly/llm/StructuredOutput.h"
namespace firefly::llm {
enum class CommandProtocol { Structured, ScopedCompact, StableCompact };
struct SelectedAction { std::string targetId, actionId; };
// Deterministic grounding against one immutable catalog. Throws on ambiguity.
SelectedAction selectAction(const runtime::DomainView& domain, std::string_view original);
bool compactSupported(const runtime::DomainView& domain, const SelectedAction& selected);
std::string compactRules();
std::string compactContract(const runtime::DomainView& domain, const SelectedAction& selected);
std::string compactContext(const runtime::DomainView& domain, const SelectedAction& selected);
class CompactActionDecoder final {
public:
    CompactActionDecoder(const runtime::DomainView& domain, SelectedAction selected)
        : domain_(domain), selected_(std::move(selected)) {}
    // Ordered scalar [integer] / [] / null (U is also a rejection sentinel).
    // Complex schemas retain strict JSON with identity locking.
    DecodedResult decode(std::string_view raw) const;
private:
    const runtime::DomainView& domain_;
    const SelectedAction selected_;
};
}
