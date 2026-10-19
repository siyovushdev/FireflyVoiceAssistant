#pragma once
#include "firefly/runtime/DomainView.h"
#include <string_view>
namespace firefly {
class SafetyGate {
public:
    // Preserve the semantic candidate or reject; ORIGINAL input is mandatory.
    // Configuration supplies evidence data, never the deterministic safety rule.
    static actions::CandidateResult apply(std::string_view original, const actions::CandidateResult& candidate,
                                          const runtime::DomainView& domain);
    // Only for the native scoped ON/OFF decoder; all ordinary paths retain
    // their existing exact-action and full original-coverage requirements.
    static actions::CandidateResult applyPowerIntent(std::string_view original,
        const actions::CandidateResult& candidate, const runtime::DomainView& domain);
};
}
