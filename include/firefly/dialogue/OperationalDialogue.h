#pragma once
#include "firefly/config/Configuration.h"
#include "firefly/execution/DeviceStateStore.h"
#include "firefly/execution/ActionExecutor.h"
#include <functional>
#include <mutex>

namespace firefly::dialogue {
enum class Question { None, State, OnList, LastAction, Failure, Capabilities, Unclear };
struct Event {
    std::uint64_t sequence = 0, generation = 0;
    std::string source;
    execution::ExecutionResult execution;
};
// Read-only value snapshot. No executor, runtime/model handle, URL or credentials.
struct Facts {
    config::Catalog catalog;
    std::uint64_t generation = 0;
    std::int64_t steadyMs = 0;
    std::vector<execution::DeviceKnowledge> devices;
    std::vector<Event> history;
    bool modelReady = false;
    // Failed explicit reads must not turn older cached data into a fresh answer.
    std::vector<std::string> failedReads;
};
struct Reply {
    bool handled = false;
    std::string text, reason;
    Question question = Question::None;
};
class OperationalDialogue final {
public:
    using Classifier = std::function<Question(std::string_view)>;
    using Refresh = std::function<Facts(const std::vector<std::string>&)>;
    // Classifier can only choose a read-only question kind. Entity identities
    // and all spoken facts come exclusively from the supplied snapshot.
    Reply ask(std::string_view original, const Facts& facts, bool enabled,
              bool detailed, const Classifier& classify = {}, const Refresh& refresh = {});
    std::string commandReply(const Facts& facts, const std::optional<execution::ExecutionResult>& execution,
                             const std::string& rejection, bool detailed, std::optional<std::int64_t> after = std::nullopt,
                             const actions::GenericAction* command = nullptr);
    static bool executionConfirmed(const Facts& facts, const execution::ExecutionResult& execution,
                                   const actions::GenericAction& command, std::int64_t notBefore);
    void rememberAttempt(const Facts& facts, const std::optional<execution::ExecutionResult>& execution,
                         const std::string& rejection, std::string_view original = {}, std::string feedback = {});
    void clear();
    static bool questionLike(std::string_view original);
private:
    struct Context {
        std::string hash, target, location;
        std::int64_t at = 0;
        int turns = 0;
        Question question = Question::None;
    };
    struct Attempt {
        std::string hash, rejection, target;
        std::optional<execution::ExecutionResult> execution;
        std::uint64_t seenSequence = 0;
        std::string feedback;
    };
    std::mutex mutex_;
    Context context_;
    std::optional<Attempt> attempt_;
};
}
