#include "firefly/CommandEngine.h"
#include "firefly/ActionValidator.h"
#include "firefly/DeterministicParser.h"
#include "firefly/SafetyGate.h"
#include "firefly/InputPolicy.h"
#include <chrono>
#include <exception>

namespace firefly {
CommandEngine::CommandEngine(LlmEngine& llm, const DeterministicParser& parser)
    : llm_(llm), parse_([&parser](std::string_view original) { return parser.parseChecked(original); }) {}
CommandEngine::CommandEngine(LlmEngine& llm)
    : llm_(llm), parse_(DeterministicParser::parse) {}
CommandResult CommandEngine::process(std::string_view original) {
    const auto start = std::chrono::steady_clock::now();
    CommandResult result{};
    result.path = CommandPath::InputRejected;
    try {
        if (!validInput(original)) {
            result.error = "Invalid input: expected nonempty UTF-8, at most 192 bytes";
        } else {
            auto candidate = parse_(original);
            result.path = candidate ? CommandPath::FastPath : CommandPath::LlmFallback;
            if (!candidate) {
                result.rawOutput = llm_.infer(original);
                candidate = std::string{};
                for (char c : result.rawOutput) {
                    if (std::string_view(" \t\r\n\f\v").find(c) == std::string_view::npos)
                        candidate->push_back(c);
                }
                result.normalizedOutput = *candidate;
                result.invalidModelOutput = *candidate != "U" && !ActionValidator::validate(*candidate);
            }
            result.action = ActionValidator::validate(SafetyGate::apply(original, *candidate));
        }
    } catch (const std::exception& error) {
        result.action.reset();
        result.error = error.what();
    } catch (...) {
        result.action.reset();
        result.error = "Unknown engine failure";
    }
    result.latencyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return result;
}
}
