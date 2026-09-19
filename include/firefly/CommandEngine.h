#pragma once
#include "firefly/DeterministicParser.h"
#include "firefly/LlmEngine.h"
#include "firefly/runtime/DomainView.h"
#include "firefly/llm/StructuredOutput.h"
namespace firefly {
enum class CommandPath { FastPath, LlmFallback, InputRejected };
struct CommandResult {
    CommandPath path = CommandPath::InputRejected;
    actions::CandidateResult action;
    // Diagnostic model bytes only; never the authoritative action representation.
    std::string rawOutput;
    llm::DecodeStatus decodeStatus = llm::DecodeStatus::NotAttempted;
    actions::CandidateResult candidate;
    actions::CandidateResult safeCandidate;
    bool safetyRejected = false;
    bool domainRejected = false;
    std::string error;
    bool invalidModelOutput = false;
    double latencyMs = 0;
};
class CommandEngine {
public:
    // Explicit immutable dependencies must outlive this engine. No legacy/default
    // constructor, alternative authoritative path or snapshot file access.
    CommandEngine(LlmEngine& llm, const DeterministicParser& parser, const runtime::DomainView& domain);
    CommandResult process(std::string_view original);
private:
    LlmEngine& llm_;
    const DeterministicParser& parser_;
    const runtime::DomainView& domain_;
    const llm::StructuredDecoder modelDecoder_;
};
}
