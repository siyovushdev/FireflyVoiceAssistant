#include "firefly/admin/RuntimeGeneration.h"
#include <atomic>
#include <chrono>

namespace firefly::admin {
RuntimeGeneration::RuntimeGeneration(config::Catalog catalog, std::uint64_t generationId)
    : id(generationId), executor(std::make_shared<execution::MockDeviceExecutor>(catalog)), domain(std::move(catalog)), parser(domain), prompt(llm::generateContext(domain).systemPrompt()) {
    // Enabling editable deny/disable policy would be a separate consumer change.
    // Do not accept configuration whose policy the current pipeline ignores.
    for (const auto& effective : domain.snapshot()->effectiveActions())
        if (!effective.permitted)
            throw config::ConfigError("unsupported_runtime_policy", "/safety", "This runtime requires enabled devices/actions and no deny restrictions. Deterministic SafetyGate remains mandatory.");
    for (const auto& definition : domain.snapshot()->configuration().actions)
        if (!definition.enabled)
            throw config::ConfigError("unsupported_runtime_policy", "/actions", "Disabled action definitions are not supported by this runtime.");
    for (const auto& target : domain.snapshot()->configuration().devices)
        if (target.kind == config::DeviceKind::EntranceDoor && target.id != domain.entranceTarget()->id)
            throw config::ConfigError("unsupported_runtime_target", "/devices", "Only the configured entrance-door target is supported; additional door targets cannot be applied.");
}
std::shared_ptr<const RuntimeGeneration> RuntimeGenerations::current() const {
    return std::atomic_load(&active_);
}
namespace {
struct Busy {
    std::atomic<bool>& flag;
    explicit Busy(std::atomic<bool>& flag) : flag(flag) { flag = true; }
    ~Busy() { flag = false; }
};
}
void RuntimeGenerations::apply(config::Catalog catalog) {
    // Pure admission outside the serialization lock; no partially built state is published.
    RuntimeGeneration admission(catalog);
    std::lock_guard<std::mutex> lock(operation_);
    Busy busy(busy_);
    if (stopping_) throw std::runtime_error("Runtime stopping");
    const auto next = std::make_shared<const RuntimeGeneration>(std::move(catalog), nextGeneration_);
    if (const auto old = current()) next->executor->reconcile(old->executor->states());
    try { model_.useContext(next->prompt); ready_ = model_.ready(); }
    catch (...) { ready_ = model_.ready(); throw; }
    std::atomic_store(&active_, next);
    ++nextGeneration_;
}
void RuntimeGenerations::preflight(config::Catalog catalog) {
    const auto next = std::make_shared<const RuntimeGeneration>(std::move(catalog));
    std::lock_guard<std::mutex> lock(operation_);
    Busy busy(busy_);
    const auto old = current();
    if (!old) throw std::runtime_error("No active runtime for context preflight");
    next->executor->reconcile(old->executor->states());
    try { model_.useContext(next->prompt); model_.useContext(old->prompt); ready_ = model_.ready(); }
    catch (...) { ready_ = model_.ready(); throw; }
}
RuntimeGenerations::Request::Request(RuntimeGenerations& owner)
    : owner_(owner), lock_(owner.operation_), generation_(owner.current()) {
    if (owner.stopping_ || !generation_) throw std::runtime_error("Runtime is not active or is stopping");
    owner_.busy_ = true;
}
RuntimeGenerations::Request::~Request() { owner_.busy_ = false; }
std::unique_ptr<RuntimeGenerations::Request> RuntimeGenerations::beginRequest() {
    return std::unique_ptr<Request>(new Request(*this));
}
GenerationResult RuntimeGenerations::process(std::string_view input, bool execute, std::string source) {
    return beginRequest()->process(input, execute, std::move(source));
}
GenerationResult RuntimeGenerations::Request::process(std::string_view input, bool execute, std::string source) {
    if (processed_) throw std::logic_error("Request already consumed");
    processed_ = true;
    const auto& generation = generation_;
    // Bind every fallback to the admitted generation, including recovery after
    // an unsuccessful preflight restoration. Fast-path commands need no model.
    class BoundModel final : public LlmEngine {
    public:
        BoundModel(ContextModel& model, const std::string& prompt) : model_(model), prompt_(prompt) {}
        std::string infer(std::string_view original) override {
            model_.useContext(prompt_);
            return model_.infer(original);
        }
    private:
        ContextModel& model_;
        const std::string& prompt_;
    } bound(owner_.model_, generation->prompt);
    CommandEngine engine(bound, generation->parser, generation->domain);
    GenerationResult result{generation, engine.process(input), std::nullopt};
    owner_.ready_ = owner_.model_.ready();
    if (execute && !owner_.stopping_) {
        if (const auto* action = std::get_if<actions::GenericAction>(&result.command.action)) {
            result.execution = generation->executor->execute(*action);
            std::lock_guard<std::mutex> historyLock(owner_.historyMutex_);
            owner_.history_.push_back({owner_.nextEvent_++, generation->id,
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(),
                std::move(source), *result.execution});
            if (owner_.history_.size() > 100) owner_.history_.pop_front();
        }
    }
    return result;
}
std::vector<ExecutionEvent> RuntimeGenerations::history() const {
    std::lock_guard<std::mutex> lock(historyMutex_);
    return {history_.begin(), history_.end()};
}
}
