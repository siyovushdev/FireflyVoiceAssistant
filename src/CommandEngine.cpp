#include "firefly/CommandEngine.h"
#include "firefly/ActionValidator.h"
#include "firefly/DeterministicParser.h"
#include "firefly/SafetyGate.h"
#include "firefly/InputPolicy.h"
#include "firefly/LightBrightnessDomain.h"
#include <chrono>
#include <exception>

namespace firefly {
CommandEngine::CommandEngine(LlmEngine& llm, const DeterministicParser& parser,
                             const LightBrightnessDomain& brightnessDomain,
                             const EntranceDoorDomain& entranceDomain)
    : llm_(llm), parse_([&parser](std::string_view original) { return parser.parseChecked(original); }),
      brightnessDomain_(&brightnessDomain), entranceDomain_(&entranceDomain) {}
CommandEngine::CommandEngine(LlmEngine& llm, const DeterministicParser& parser,
                             const LightBrightnessDomain& brightnessDomain)
    : llm_(llm), parse_([&parser](std::string_view original) { return parser.parseChecked(original); }),
      brightnessDomain_(&brightnessDomain), entranceDomain_(nullptr) {}
CommandEngine::CommandEngine(LlmEngine& llm, const DeterministicParser& parser)
    : llm_(llm), parse_([&parser](std::string_view original) { return parser.parseChecked(original); }),
      brightnessDomain_(nullptr), entranceDomain_(nullptr) {}
CommandEngine::CommandEngine(LlmEngine& llm)
    : llm_(llm), parse_(DeterministicParser::parse), brightnessDomain_(nullptr), entranceDomain_(nullptr) {}
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
            const auto gated = SafetyGate::apply(original, *candidate);
            auto validated = ActionValidator::validate(gated);
            if (validated && brightnessDomain_) {
                if (const auto* light = std::get_if<LightAction>(&*validated)) {
                    // The strict decoder established gated[2] as a light code.
                    // Protocol/safety validity alone does not establish support
                    // by this installation. Never clamp or retry a domain miss.
                    if (!brightnessDomain_->supportsBrightness(gated[2], light->brightness)) {
                        validated.reset();
                        result.error = "Brightness outside configured domain";
                    }
                }
            }
            if (validated && entranceDomain_ && std::holds_alternative<OpenEntranceDoor>(*validated)) {
                // Safety/strict decoding establish intent and representation;
                // only the snapshot-backed resolver supplies installation identity.
                result.entranceTarget = entranceDomain_->resolveEntranceDoor();
                if (!result.entranceTarget) {
                    validated.reset();
                    result.error = "Configured entrance-door target unavailable";
                }
            }
            result.action = std::move(validated);
        }
    } catch (const std::exception& error) {
        result.action.reset();
        result.entranceTarget.reset();
        result.error = error.what();
    } catch (...) {
        result.action.reset();
        result.entranceTarget.reset();
        result.error = "Unknown engine failure";
    }
    result.latencyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return result;
}
}
