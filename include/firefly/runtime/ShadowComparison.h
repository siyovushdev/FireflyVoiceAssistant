#pragma once
#include "firefly/runtime/DomainView.h"

namespace firefly::runtime {
struct ShadowComparison {
    std::size_t checks = 0;
    std::vector<std::string> mismatches;
    bool equivalent() const { return mismatches.empty(); }
    // Test/harness assertion only. Production records mismatches observationally.
    void requireEquivalent() const;
};
// Fixed, synthetic startup probes only; never interprets a user's command.
// This adapter depends on the old components; the configuration library does not.
ShadowComparison compareLegacyDomain(const DomainView& domain);
}
