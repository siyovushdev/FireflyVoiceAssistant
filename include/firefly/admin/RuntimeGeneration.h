#pragma once
#include "firefly/CommandEngine.h"
#include "firefly/llm/LlmContext.h"
#include <memory>
#include <mutex>

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
    explicit RuntimeGeneration(config::Catalog catalog);
    const runtime::DomainView domain;
    const DeterministicParser parser;
    const std::string prompt;
};
struct GenerationResult {
    std::shared_ptr<const RuntimeGeneration> generation;
    CommandResult command;
};
class RuntimeGenerations final {
public:
    explicit RuntimeGenerations(ContextModel& model) : model_(model) {}
    std::shared_ptr<const RuntimeGeneration> current() const;
    void apply(config::Catalog catalog);
    void preflight(config::Catalog catalog);
    GenerationResult process(std::string_view input);
private:
    ContextModel& model_;
    std::shared_ptr<const RuntimeGeneration> active_;
    // Synchronous RKLLM: finish admitted old requests before context refresh.
    std::mutex operation_;
};
}
