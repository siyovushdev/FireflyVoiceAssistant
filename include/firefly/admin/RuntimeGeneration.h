#pragma once
#include "firefly/CommandEngine.h"
#include "firefly/llm/LlmContext.h"
#include <memory>
#include <mutex>
#include <atomic>
#include <deque>
#include "firefly/execution/ActionExecutor.h"

namespace firefly::admin {
// The model adapter refreshes context transactionally, retaining/restoring the
// old context on failure. No model implementation or persistence in this layer.
class ContextModel : public LlmEngine {
public:
    virtual void useContext(const std::string& prompt) = 0;
    virtual bool ready() const = 0;
};
class RuntimeGeneration final {
public:
    explicit RuntimeGeneration(config::Catalog catalog, std::uint64_t id = 0);
    const std::uint64_t id;
    const std::shared_ptr<execution::MockDeviceExecutor> executor;
    const runtime::DomainView domain;
    const DeterministicParser parser;
    const std::string prompt;
};
struct GenerationResult {
    std::shared_ptr<const RuntimeGeneration> generation;
    CommandResult command;
    std::optional<execution::ExecutionResult> execution;
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
    class Request final {
    public:
        ~Request();
        GenerationResult process(std::string_view input, bool execute = false, std::string source = "text");
        const std::shared_ptr<const RuntimeGeneration>& generation() const { return generation_; }
    private:
        friend class RuntimeGenerations;
        explicit Request(RuntimeGenerations& owner);
        RuntimeGenerations& owner_;
        std::unique_lock<std::mutex> lock_;
        std::shared_ptr<const RuntimeGeneration> generation_;
        bool processed_ = false;
    };
    std::unique_ptr<Request> beginRequest();
    GenerationResult process(std::string_view input, bool execute = false, std::string source = "text");
    std::vector<ExecutionEvent> history() const;
    bool modelReady() const { return ready_.load(); }
    bool busy() const { return busy_.load(); }
    void stop() { stopping_ = true; }
private:
    friend class Request;
    std::atomic<bool> ready_{false}, busy_{false}, stopping_{false};
    std::uint64_t nextGeneration_ = 1, nextEvent_ = 1;
    mutable std::mutex historyMutex_;
    std::deque<ExecutionEvent> history_;
    ContextModel& model_;
    std::shared_ptr<const RuntimeGeneration> active_;
    // Synchronous RKLLM: finish admitted old requests before context refresh.
    std::mutex operation_;
};
}
