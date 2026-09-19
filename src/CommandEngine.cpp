#include "firefly/CommandEngine.h"
#include "firefly/SafetyGate.h"
#include "firefly/InputPolicy.h"
#include <chrono>
#include <exception>
namespace firefly {
CommandEngine::CommandEngine(LlmEngine& llm, const DeterministicParser& parser, const runtime::DomainView& domain)
    : llm_(llm), parser_(parser), domain_(domain), modelCodec_(domain.snapshot()) {}
CommandResult CommandEngine::process(std::string_view original) {
    const auto start = std::chrono::steady_clock::now();
    CommandResult result;
    try {
        if (!validInput(original)) {
            result.error = "Invalid input: expected nonempty UTF-8, at most 192 bytes";
        } else {
            auto candidate = parser_.parse(original);
            result.path = std::holds_alternative<actions::GenericAction>(candidate) ? CommandPath::FastPath : CommandPath::LlmFallback;
            if (result.path == CommandPath::LlmFallback) {
                // Frozen RKLLM compatibility ingress. Normalize only model bytes,
                // decode once, then leave strings behind except in diagnostics.
                result.rawOutput = llm_.infer(original);
                for (const char c : result.rawOutput)
                    if (std::string_view(" \t\r\n\f\v").find(c) == std::string_view::npos)
                        result.normalizedOutput.push_back(c);
                const auto decoded = modelCodec_.decode(result.normalizedOutput);
                if (const auto* value = std::get_if<actions::CandidateResult>(&decoded)) candidate = *value;
                else { candidate = actions::Unsupported{}; result.invalidModelOutput = true; }
            }
            const auto safe = SafetyGate::apply(original, candidate, domain_);
            result.action = domain_.validate(safe);
            if (std::holds_alternative<actions::GenericAction>(safe) &&
                std::holds_alternative<actions::Unsupported>(result.action))
                result.error = "Action outside configured domain";
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
