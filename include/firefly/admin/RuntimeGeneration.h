#pragma once
#include "firefly/CommandEngine.h"
#include "firefly/llm/LlmContext.h"
#include "firefly/llm/CompactActionDecoder.h"
#include <memory>
#include <mutex>
#include <atomic>
#include <deque>
#include <functional>
#include "firefly/execution/ActionExecutor.h"

namespace firefly::admin {
// The model adapter refreshes context transactionally, retaining/restoring the
// old context on failure. No model implementation or persistence in this layer.
class ContextModel : public LlmEngine {
public:
    virtual llm::CommandProtocol commandProtocol() const { return llm::CommandProtocol::Structured; }
    virtual std::string inferCommand(std::string_view original, std::string_view) { return infer(original); }
    virtual void useContext(const std::string& prompt) = 0;
    virtual bool ready() const = 0;
    virtual int contextTokens() const { return -1; }
    virtual void useAdminContext(const std::string& prompt, int) { useContext(prompt); }
    virtual bool setupRequired() const { return false; }
    virtual void enterSetup() { throw std::runtime_error("Setup profile unavailable"); }
    virtual void leaveSetup(const std::string& prompt) { useContext(prompt); }
    virtual std::string inferAdmin(std::string_view text) { return infer(text); }
    virtual std::string inferenceMetrics() const { return "{}"; }
};
class RuntimeGeneration final {
public:
    explicit RuntimeGeneration(config::Catalog catalog, std::uint64_t id = 0, llm::CommandProtocol protocol = llm::CommandProtocol::Structured);
    const std::uint64_t id;
    const llm::CommandProtocol protocol;
    // Always BoundDeviceExecutor. Playground vs Production is chosen per request via
    // ExecutionIntent — never by bindings.empty() or diagnostic source strings.
    const std::shared_ptr<execution::BoundDeviceExecutor> executor;
    const runtime::DomainView domain;
    const DeterministicParser parser;
    const std::shared_ptr<const language::LanguageIndex> languageIndex;
    const std::string prompt;
    const std::map<std::string, std::string> targetContexts;
    std::string defaultPrompt() const;
    std::string requestPrompt(std::string_view original) const;
};
struct GenerationResult {
    std::shared_ptr<const RuntimeGeneration> generation;
    CommandResult command;
    std::optional<execution::ExecutionResult> execution;
    std::string guardError;
};
struct CommandGuard {
    std::string expectedHash;
    actions::GenericAction expectedAction;
};
struct ExecutionEvent {
    std::uint64_t sequence, generation;
    std::int64_t timestampMs;
    std::string source;
    execution::ExecutionResult result;
};
class RuntimeGenerations final {
public:
    explicit RuntimeGenerations(ContextModel& model) : model_(model) {}
    std::shared_ptr<const RuntimeGeneration> current() const;
    void apply(config::Catalog catalog);
    void preflight(config::Catalog catalog);
    std::string suggest(std::string_view entity, std::string_view name);
    std::string draft(std::string_view entity, std::string_view name);
    using SuggestionDecoder = std::function<std::string(std::string_view)>;
    std::string structuredSuggestion(const std::string& prompt, std::string_view input, int budget,
                                    const SuggestionDecoder& decode);
    class Request final {
    public:
        ~Request();
        GenerationResult process(std::string_view input,
            execution::ExecutionIntent execution = execution::ExecutionIntent::None,
            std::string source = "text", const CommandGuard* guard = nullptr);
        std::string classifyOperationalQuestion(std::string_view original, CommandTimings* timings = nullptr);
        const std::shared_ptr<const RuntimeGeneration>& generation() const { return generation_; }
    private:
        friend class RuntimeGenerations;
        explicit Request(RuntimeGenerations& owner);
        RuntimeGenerations& owner_;
        std::unique_lock<std::timed_mutex> lock_;
        std::shared_ptr<const RuntimeGeneration> generation_;
        bool processed_ = false;
    };
    std::unique_ptr<Request> beginRequest();
    GenerationResult process(std::string_view input,
        execution::ExecutionIntent execution = execution::ExecutionIntent::None,
        std::string source = "text");
    // Simulated independent telemetry into the active generation's DeviceStateStore.
    void observe(execution::StateObservation observation);
    execution::HardwareResult poll(const std::string& targetId, const std::string& expectedHash);
    std::vector<ExecutionEvent> history() const;
    bool modelReady() const { return ready_.load(); }
    int contextTokens() const { return contextTokens_.load(); }
    bool busy() const { return busy_.load(); }
    enum class SetupMode { Running, Entering, Active, Restoring, Degraded };
    SetupMode setupMode() const { return setupMode_.load(); }
    bool setupBlocked() const { return setupMode() != SetupMode::Running; }
    const char* setupModeName() const;
    void enterSetup();
    void prepareRestore();
    void finishRestore();
    void setAudioDrain(std::function<void()> drain) { audioDrain_ = std::move(drain); }
    void stop() { stopping_ = true; }
private:
    friend class Request;
    std::atomic<std::uint64_t> setupEpoch_{0};
    std::unique_lock<std::timed_mutex> lockRequest();
    std::atomic<SetupMode> setupMode_{SetupMode::Running};
    std::function<void()> audioDrain_;
    std::atomic<bool> ready_{false}, busy_{false}, stopping_{false};
    std::atomic<int> contextTokens_{-1};
    std::uint64_t nextGeneration_ = 1, nextEvent_ = 1;
    mutable std::mutex historyMutex_;
    std::deque<ExecutionEvent> history_;
    ContextModel& model_;
    std::shared_ptr<const RuntimeGeneration> active_;
    // Synchronous RKLLM: finish admitted old requests before context refresh.
    std::timed_mutex operation_;
    std::map<std::string,int> admittedContexts_; // Bounded successful prompt admissions, never KV or persistent state.
    int admitContext(const std::string& prompt);
    std::unique_lock<std::timed_mutex> lockOperation();
};
}
