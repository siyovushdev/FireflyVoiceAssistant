#include "firefly/CommandEngine.h"
#include "firefly/SafetyGate.h"
#include "firefly/InputPolicy.h"
#include <chrono>
#include <exception>
namespace firefly {
CommandEngine::CommandEngine(LlmEngine& llm, const DeterministicParser& parser, const runtime::DomainView& domain)
    : llm_(llm), parser_(parser), domain_(domain), modelDecoder_(domain) {}
CommandResult CommandEngine::process(std::string_view original) {
    const auto start = std::chrono::steady_clock::now();
    CommandResult result;
    try {
        if (!validInput(original)) {
            result.error = "Invalid input: expected nonempty UTF-8, at most 192 bytes";
        } else {
            result.grounding = domain_.resolve(original);
            auto candidate = parser_.parse(original);
            if (std::holds_alternative<actions::Unsupported>(candidate)) candidate = domain_.parseDeclared(original);
            result.path = std::holds_alternative<actions::GenericAction>(candidate) ? CommandPath::FastPath : CommandPath::LlmFallback;
            if (result.path == CommandPath::LlmFallback) {
                // Structured ingress: preserve raw bytes, with no whitespace or
                // transcript repair. JSON whitespace is handled by the decoder.
                result.rawOutput = llm_.infer(original);
                const auto decoded = modelDecoder_.decode(result.rawOutput);
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
        if (error.code == "ambiguous_target" || error.code == "target_not_grounded") result.path = CommandPath::GroundingRejected;
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
