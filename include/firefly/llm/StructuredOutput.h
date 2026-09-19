#pragma once
#include "firefly/runtime/DomainView.h"
#include <string_view>

namespace firefly::llm {
inline constexpr std::size_t MaxStructuredBytes = 4096;
enum class DecodeStatus {
    NotAttempted, Action, Unsupported, InvalidJson, InvalidShape,
    UnknownAction, UnknownTarget, InvalidArguments, OutsideDomain
};
const char* decodeStatusName(DecodeStatus status);
struct DecodedResult {
    actions::CandidateResult value;
    DecodeStatus status = DecodeStatus::InvalidJson;
    bool valid() const { return status == DecodeStatus::Action || status == DecodeStatus::Unsupported; }
};
// The read-only DomainView must outlive this decoder (owned by RuntimeConfig).
class StructuredDecoder final {
public:
    explicit StructuredDecoder(const runtime::DomainView& domain) : domain_(domain) {}
    DecodedResult decode(std::string_view raw) const;
private:
    const runtime::DomainView& domain_;
};
// Exact semantic JSON for diagnostics/tests; never repairs or authorizes a value.
std::string structuredResult(const actions::CandidateResult& value);
std::string quotedText(std::string_view value);
}
