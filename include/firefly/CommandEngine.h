#pragma once
#include "firefly/DeterministicParser.h"
#include "firefly/LlmEngine.h"
#include "firefly/runtime/DomainView.h"
#include "firefly/llm/StructuredOutput.h"
#include <functional>
namespace firefly {
enum class CommandPath { FastPath, LlmFallback, InputRejected, GroundingRejected };
struct CommandTimings {
    double contextMs = 0, prefillMs = 0, generationMs = 0;
    int outputTokens = 0;
};
struct CommandResult {
    CommandPath path = CommandPath::InputRejected;
    actions::CandidateResult action;
    // Diagnostic model bytes only; never the authoritative action representation.
    std::string rawOutput;
    runtime::Grounding grounding;
    llm::DecodeStatus decodeStatus = llm::DecodeStatus::NotAttempted;
    actions::CandidateResult candidate;
    actions::CandidateResult safeCandidate;
    bool safetyRejected = false;
    bool domainRejected = false;
    std::string error;
    std::string reason;
    bool invalidModelOutput = false;
    double latencyMs = 0;
    CommandTimings llmTimings;
};
class CommandEngine {
public:
    // Explicit immutable dependencies must outlive this engine. No legacy/default
    // constructor, alternative authoritative path or snapshot file access.
    using Decoder = std::function<llm::DecodedResult(std::string_view)>;
    CommandEngine(LlmEngine& llm, const DeterministicParser& parser, const runtime::DomainView& domain, Decoder decoder = {});
    CommandResult process(std::string_view original);
private:
    LlmEngine& llm_;
    const DeterministicParser& parser_;
    const runtime::DomainView& domain_;
    const llm::StructuredDecoder modelDecoder_;
    const Decoder decoder_;
};
}
