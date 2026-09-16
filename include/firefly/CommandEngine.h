#pragma once
#include "firefly/Action.h"
#include "firefly/LlmEngine.h"
#include <functional>
#include <optional>
namespace firefly {
class DeterministicParser;
class LightBrightnessDomain;
enum class CommandPath { FastPath, LlmFallback, InputRejected };
struct CommandResult {
    CommandPath path;
    std::optional<Action> action;
    std::string rawOutput;
    std::string normalizedOutput;
    std::string error;
    bool invalidModelOutput = false;
    double latencyMs = 0;
    std::string compact() const { return action ? toCompact(*action) : "U"; }
};
class CommandEngine {
public:
    // Production constructor. Both read-only dependencies must outlive this engine.
    CommandEngine(LlmEngine& llm, const DeterministicParser& parser,
                  const LightBrightnessDomain& brightnessDomain);
    // Migration-only 6.4 reference for existing standalone tests.
    CommandEngine(LlmEngine& llm, const DeterministicParser& parser);
    // Migration-only reference constructor for unchanged legacy tests/harness.
    // The injected production path cannot switch to this implementation.
    explicit CommandEngine(LlmEngine& llm);
    // Fail closed: every failure returns no action and diagnostic metadata.
    CommandResult process(std::string_view original);
private:
    LlmEngine& llm_;
    const std::function<std::optional<std::string>(std::string_view)> parse_;
    // Null only in explicitly selected historical reference constructors.
    const LightBrightnessDomain* const brightnessDomain_;
};
}
