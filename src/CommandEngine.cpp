#include "firefly/CommandEngine.h"
#include "firefly/SafetyGate.h"
#include "firefly/InputPolicy.h"
#include <chrono>
#include <exception>
#include <stdexcept>
namespace firefly {
CommandEngine::CommandEngine(LlmEngine& llm, const DeterministicParser& parser, const runtime::DomainView& domain, Decoder decoder, const language::LanguageIndex* index)
    : llm_(llm), parser_(parser), domain_(domain), modelDecoder_(domain), decoder_(std::move(decoder)), index_(index) {
    if (index_ && index_->catalogHash() != domain_.snapshot()->contentHash())
        throw std::invalid_argument("LanguageIndex and DomainView must share one configuration snapshot");
}
CommandResult CommandEngine::process(std::string_view original) {
    const auto start = std::chrono::steady_clock::now();
    CommandResult result;
    try {
        if (!validInput(original)) {
            result.languageRoute = "INPUT_REJECTED";
            result.indexStatus = "INDEX_INVALID";
            result.error = "Invalid input: expected nonempty UTF-8, at most 192 bytes";
        } else {
            result.grounding = domain_.resolve(original);
            language::MatchResult indexed;
            if(index_)indexed=index_->match(original);
            result.indexMatch=indexed;
            result.indexStatus=language::matchName(indexed.kind);
            if(indexed.kind==language::MatchKind::Ambiguous||indexed.kind==language::MatchKind::Invalid) {
                result.path=CommandPath::GroundingRejected;result.languageRoute=result.indexStatus;result.reason=indexed.reason;
                result.latencyMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();return result;
            }
            auto candidate = indexed.candidate;
            const bool hit=std::holds_alternative<actions::GenericAction>(candidate);
            if(!hit)candidate=parser_.parse(original);
            if (std::holds_alternative<actions::Unsupported>(candidate)) candidate = domain_.parseDeclared(original);
            result.path = std::holds_alternative<actions::GenericAction>(candidate) ? CommandPath::FastPath : CommandPath::LlmFallback;
            result.languageRoute = hit ? result.indexStatus : result.path==CommandPath::FastPath ? "LEGACY_FAST_PATH" : "LLM_FALLBACK";
            if (result.path == CommandPath::LlmFallback) {
                // Preserve raw model bytes; the selected strict decoder handles framing.
                // No transcript repair or output rewriting.
                result.rawOutput = llm_.infer(original);
                const auto decoded = decoder_ ? decoder_(result.rawOutput) : modelDecoder_.decode(result.rawOutput);
                candidate = decoded.value;
                result.decodeStatus = decoded.status;
                result.reason = decoded.reason;
                result.invalidModelOutput = !decoded.valid();
            }
            result.candidate = candidate;
            const auto safe = SafetyGate::apply(original, candidate, domain_);
            result.safeCandidate = safe;
            result.safetyRejected = std::holds_alternative<actions::GenericAction>(candidate) &&
                                    std::holds_alternative<actions::Unsupported>(safe);
            result.action = domain_.validate(safe);
            if (std::holds_alternative<actions::GenericAction>(safe) &&
                std::holds_alternative<actions::Unsupported>(result.action)) {
                result.domainRejected = true;
                result.error = "Action outside configured domain"; // Legacy CLI diagnostics.
                result.reason = domain_.validationReason(std::get<actions::GenericAction>(safe));
            }
        }
    } catch (const config::ConfigError& error) {
        result.action = actions::Unsupported{};
        result.error = error.code + ": " + error.what();
        if (error.code == "ambiguous_target" || error.code == "ambiguous_action" || error.code == "target_not_grounded") {
            result.path = CommandPath::GroundingRejected;
            result.languageRoute = "GROUNDING_REJECTED";
        }
    } catch (const std::exception& error) {
        result.action = actions::Unsupported{};
        result.error = error.what();
    } catch (...) {
        result.action = actions::Unsupported{};
        result.error = "Unknown engine failure";
    }
    result.latencyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return result;
}
}
