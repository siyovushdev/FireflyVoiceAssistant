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
};
}
