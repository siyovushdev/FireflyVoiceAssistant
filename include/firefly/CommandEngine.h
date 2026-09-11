#pragma once
#include "firefly/Action.h"
#include "firefly/LlmEngine.h"
#include <optional>
namespace firefly {
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
    explicit CommandEngine(LlmEngine& llm) : llm_(llm) {}
    // Fail closed: every failure returns no action and diagnostic metadata.
    CommandResult process(std::string_view original);
private:
    LlmEngine& llm_;
};
}
