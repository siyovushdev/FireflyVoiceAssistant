#include "firefly/CommandEngine.h"
#include "firefly/ActionValidator.h"
#include "firefly/DeterministicParser.h"
#include "firefly/SafetyGate.h"

namespace firefly {
CommandResult CommandEngine::process(std::string_view original) {
    auto candidate = DeterministicParser::parse(original);
    const auto path = candidate ? CommandPath::FastPath : CommandPath::LlmFallback;
    if (!candidate) {
        const auto raw = llm_.infer(original);
        candidate = std::string{};
        // Match reference removeWhitespace, explicitly using ASCII whitespace.
        for (char c : raw) {
            if (std::string_view(" \t\r\n\f\v").find(c) == std::string_view::npos)
                candidate->push_back(c);
        }
    }
    return {path, ActionValidator::validate(SafetyGate::apply(original, *candidate))};
}
}
