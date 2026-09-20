#include "firefly/admin/RuntimeGeneration.h"
#include <atomic>

namespace firefly::admin {
RuntimeGeneration::RuntimeGeneration(config::Catalog catalog)
    : domain(std::move(catalog)), parser(domain), prompt(llm::generateContext(domain).systemPrompt()) {
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
void RuntimeGenerations::apply(config::Catalog catalog) {
    const auto next = std::make_shared<const RuntimeGeneration>(std::move(catalog));
    std::lock_guard<std::mutex> lock(operation_);
    model_.useContext(next->prompt);
    std::atomic_store(&active_, next);
}
void RuntimeGenerations::preflight(config::Catalog catalog) {
    const auto next = std::make_shared<const RuntimeGeneration>(std::move(catalog));
    std::lock_guard<std::mutex> lock(operation_);
    const auto old = current();
    if (!old) throw std::runtime_error("No active runtime for context preflight");
    model_.useContext(next->prompt);
    model_.useContext(old->prompt);
}
GenerationResult RuntimeGenerations::process(std::string_view input) {
    std::lock_guard<std::mutex> lock(operation_);
    const auto generation = current();
    if (!generation) throw std::runtime_error("Runtime is not active");
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
    } bound(model_, generation->prompt);
    CommandEngine engine(bound, generation->parser, generation->domain);
    return {generation, engine.process(input)};
}
}
