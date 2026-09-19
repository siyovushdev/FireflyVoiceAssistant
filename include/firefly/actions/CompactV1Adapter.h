#pragma once
#include "firefly/actions/GenericAction.h"
#include "firefly/config/Configuration.h"
#include <string_view>

namespace firefly::actions {
enum class CompactV1Error {
    InvalidSyntax,
    UnknownAction,
    UnknownTarget,
    InvalidArguments,
    Unrepresentable
};

// Syntax failure differs from an explicit U. Neither alternative is an Action.
using CompactDecodeResult = std::variant<CandidateResult, CompactV1Error>;
using CompactEncodeResult = std::variant<std::string, CompactV1Error>;

// Offline compatibility codec bound to one explicitly supplied validated snapshot.
// It checks representation/identity, not ORIGINAL-input safety, configured range
// support or enabled/deny policy. Success is a candidate, never authorization.
// No normalization, file access, model invocation, fallback mapping or JSON API.
class CompactV1Adapter final {
public:
    explicit CompactV1Adapter(config::Catalog snapshot);
    const config::Catalog& snapshot() const { return snapshot_; }
    CompactDecodeResult decode(std::string_view compact) const;
    // Only actions are encodable here. Rendering Unsupported as U belongs to a
    // presentation boundary; encoding failure must not become successful "U".
    CompactEncodeResult encode(const GenericAction& action) const;
private:
    const config::Catalog snapshot_;
};
}
