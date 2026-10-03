#include "firefly/execution/HardwareExecutor.h"
#include <stdexcept>

namespace firefly::execution {
BindingResolver::BindingResolver(config::Catalog catalog) : catalog_(std::move(catalog)) {}

BindingResolver::Result BindingResolver::resolve(const std::string& targetId, const std::string& actionId) const {
    Result result;
    if (!catalog_) return result;
    const auto& index = catalog_->bindingIndex();
    const auto key = std::make_pair(targetId, actionId);
    const auto found = index.find(key);
    if (found == index.end()) return result;
    // Validated catalogs admit at most one binding per key. Ambiguity is retained
    // as a fail-closed runtime guard if an unvalidated catalog is constructed.
    std::size_t matches = 0;
    const config::Binding* binding = nullptr;
    for (const auto& entry : catalog_->configuration().bindings) {
        if (entry.targetId == targetId && entry.actionId == actionId) {
            ++matches;
            binding = &entry;
        }
    }
    if (matches == 0) return result;
    if (matches > 1) {
        result.status = Status::Ambiguous;
        return result;
    }
    result.status = Status::Found;
    result.binding = binding;
    (void)found;
    return result;
}

ExecutorRegistry::ExecutorRegistry() = default;

void ExecutorRegistry::registerExecutor(config::ExecutorKind kind, std::shared_ptr<HardwareExecutor> executor) {
    if (kind == config::ExecutorKind::Unknown || !executor)
        throw std::invalid_argument("Executor registration requires a concrete kind and implementation");
    executors_[kind] = std::move(executor);
}

HardwareResult ExecutorRegistry::execute(const config::Binding& binding, const actions::GenericAction& action) const {
    const auto found = executors_.find(binding.executor);
    if (found == executors_.end() || !found->second)
        return {false, "unsupported_executor", {}, ReachabilityEvidence::None};
    return found->second->execute(binding, action);
}
HardwareObservation ExecutorRegistry::observe(const config::ObservationBinding& binding) const {
    const auto found = executors_.find(binding.executor);
    if (found == executors_.end()) return {{false, "unsupported_executor", {}, ReachabilityEvidence::None}, {}};
    return found->second->observe(binding);
}
}
