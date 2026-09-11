#pragma once
#include "firefly/Action.h"
#include "firefly/LlmEngine.h"
#include <optional>
namespace firefly {
enum class CommandPath { FastPath, LlmFallback };
struct CommandResult {
    CommandPath path;
    std::optional<Action> action;
    std::string compact() const { return action ? toCompact(*action) : "U"; }
};
class CommandEngine {
public:
    explicit CommandEngine(LlmEngine& llm) : llm_(llm) {}
    // Runtime failures propagate; malformed/rejected candidates return no action.
    CommandResult process(std::string_view original);
private:
    LlmEngine& llm_;
};
}
