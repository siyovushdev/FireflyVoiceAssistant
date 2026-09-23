#include "firefly/admin/RuntimeGeneration.h"
#include <atomic>
#include <chrono>
#include "firefly/admin/AdminDraft.h"
#include "Json.h"
#include "../Text.h"
#include <algorithm>

namespace firefly::admin {
namespace {
// One additional generation per user request, including multi-group drafts.
// Every attempt gets a fresh strict decode; rejected text is never repaired.
std::optional<Json> suggestionAttempts(ContextModel& model, const std::string& prompt,
        std::string_view input, int budget, const RuntimeGenerations::SuggestionDecoder& decode,
        Json& trace, int& retries, const std::atomic<bool>& stopping, const std::string& group = {}) {
    for (int attempt = 1; attempt <= 2; ++attempt) {
        if (stopping) throw std::runtime_error("Runtime stopping");
        std::string retryHint = "\nПредыдущий ответ отклонён. Повтори ПОЛНЫЙ корректный JSON строго по указанной схеме. Проверь ключи, кавычки, запятые и скобки. Без markdown и пояснений.";
        if (attempt > 1 && !trace.empty() && trace.back().contains("error") && trace.back().at("error").is_string()) {
            const auto& reason = trace.back().at("error").get_ref<const std::string&>();
            if (!reason.empty() && reason.size() <= 240) retryHint = "\nПредыдущий ответ отклонён (" + reason + "). Повтори ПОЛНЫЙ корректный JSON строго по указанной схеме. Проверь ключи, кавычки, запятые и скобки. Без markdown и пояснений.";
        }
        const auto instructions = prompt + (attempt == 1 ? "" : retryHint);
        Json entry{{"attempt",attempt},{"group",group},{"prompt",instructions},{"input",input},
            {"raw",""},{"metrics",nullptr},{"stage","context_admission"}};
        std::string raw;
        try {
            model.useAdminContext(instructions, budget);
            entry["stage"] = "inference";
            raw = model.infer(input);
            entry["raw"] = raw; entry["metrics"] = Json::parse(model.inferenceMetrics());
        } catch (const std::exception& error) {
            entry["error"] = error.what();
            entry["error_code"] = model.ready() ? "runtime_rejected" : "ai_unavailable";
            trace.push_back(std::move(entry));
            return std::nullopt; // Runtime/context failures are never format retries.
        }
        entry["stage"] = "decode";
        try {
            auto decoded = Json::parse(decode(raw));
            entry["decoded"] = decoded; trace.push_back(std::move(entry)); return decoded;
        } catch (const std::exception& error) {
            entry["error"] = error.what(); entry["error_code"] = "invalid_ai_response";
            trace.push_back(std::move(entry));
        }
        if (retries == 0 || attempt == 2) break;
        --retries;
    }
    return std::nullopt;
}
Json suggestionReply(const Json& trace, bool success, int retries) {
    Json reply{{"ok",success},{"trace",trace},{"saved",false},{"applied",false},
        {"provenance","ai_suggested"},{"retry_count",1-retries},{"retry_limit",1}};
    if (!success) reply["errors"] = failure(trace.empty() ? "invalid_ai_response" : trace.back().value("error_code",std::string("invalid_ai_response")), "",
        "AI could not prepare a valid proposal. Retry or edit manually; configuration unchanged.")["errors"];
    return reply;
}
std::map<std::string,std::string> contexts(const runtime::DomainView& domain) {
    std::map<std::string,std::string> result;
    if (domain.snapshot()->configuration().schemaVersion == 2) {
        for (const auto& device : domain.snapshot()->configuration().devices)
            if (device.kind == config::DeviceKind::Generic) {
                for (const auto& action : device.actionIds)
                    result.emplace(device.id + "/" + action,llm::generateContext(domain,device.id,action).systemPrompt());
            } else if (!device.actionIds.empty()) result.emplace(device.id,llm::generateContext(domain,device.id).systemPrompt());
    }
    return result;
}
}
std::string RuntimeGeneration::defaultPrompt() const {
    return targetContexts.empty() ? prompt : R"(Ответ только {"unsupported":true}. Сервер ещё не установил однозначную цель из полного каталога. Не выбирай устройство сам.)";
}
std::string RuntimeGeneration::requestPrompt(std::string_view original) const {
    if (targetContexts.empty()) return prompt;
    const auto grounding = domain.resolve(original);
    if (grounding.ambiguous) throw config::ConfigError("ambiguous_target", "/text", "Ambiguous devices or locations in original input");
    if (grounding.devices.size() == 1) {
        const auto& target = *domain.device(grounding.devices.front());
        std::string key = target.id;
        if (target.kind == config::DeviceKind::Generic) {
            const auto actions = domain.requestedActions(original,target);
            if (actions.size() != 1) throw config::ConfigError(actions.size()>1?"ambiguous_target":"target_not_grounded", "/text", "An exact configured action phrase is required");
            key += "/" + actions.front();
        }
        const auto context = targetContexts.find(key);
        if (context == targetContexts.end())
            throw config::ConfigError("target_not_grounded", "/text", "This device has no registered executable operation");
        return context->second;
    }
    const auto words = detail::words(original);
    const bool light = std::any_of(words.begin(),words.end(),[](const auto& w){return w=="свет"||w=="освещение"||w=="яркость"||w=="яркости";});
    if (light && grounding.locations.size() == 1) {
        std::vector<std::string> targets;
        for (const auto& device : domain.snapshot()->configuration().devices)
            if (device.roomId == grounding.locations.front() && device.kind == config::DeviceKind::Light) targets.push_back(device.id);
        if (targets.size() == 1) return targetContexts.at(targets.front());
        if (targets.size() > 1) throw config::ConfigError("ambiguous_target", "/text", "Several lights match this location");
    }
    if (detail::lowercase(original).find("двер") != std::string::npos)
        return targetContexts.at(domain.entranceTarget()->id);
    throw config::ConfigError("target_not_grounded", "/text", "No unambiguous configured device mention");
}

RuntimeGeneration::RuntimeGeneration(config::Catalog catalog, std::uint64_t generationId)
    : id(generationId), executor(std::make_shared<execution::MockDeviceExecutor>(catalog)), domain(std::move(catalog)), parser(domain), prompt(llm::generateContext(domain).systemPrompt()), targetContexts(contexts(domain)) {
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
std::unique_lock<std::timed_mutex> RuntimeGenerations::lockOperation() {
    std::unique_lock<std::timed_mutex> lock(operation_, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(45)))
        throw config::ConfigError("runtime_busy", "", "Runtime admission wait exceeded 45 seconds");
    if (stopping_) throw config::ConfigError("runtime_stopping", "", "Runtime stopping");
    return lock;
}
int RuntimeGenerations::admitContext(const std::string& prompt) {
    const auto admitted = admittedContexts_.find(prompt);
    if (admitted != admittedContexts_.end()) return admitted->second;
    model_.useContext(prompt);
    const auto tokens = model_.contextTokens();
    if (admittedContexts_.size() >= 256) admittedContexts_.clear();
    admittedContexts_.emplace(prompt,tokens);
    return tokens;
}
void RuntimeGenerations::apply(config::Catalog catalog) {
    // Pure admission outside the serialization lock; no partially built state is published.
    RuntimeGeneration admission(catalog);
    auto lock = lockOperation();
    Busy busy(busy_);
    if (stopping_) throw std::runtime_error("Runtime stopping");
    const auto next = std::make_shared<const RuntimeGeneration>(std::move(catalog), nextGeneration_);
    if (const auto old = current()) next->executor->reconcile(old->executor->states());
    int nextContextTokens = -1;
    try {
        for (const auto& [id,context] : next->targetContexts) { (void)id; nextContextTokens = std::max(nextContextTokens,admitContext(context)); }
        model_.useContext(next->defaultPrompt()); ready_ = model_.ready();
    } catch (...) {
        const auto error = std::current_exception();
        if (const auto old = current()) try { model_.useContext(old->defaultPrompt()); } catch (...) {}
        ready_ = model_.ready(); std::rethrow_exception(error);
    }
    contextTokens_ = next->targetContexts.empty() ? model_.contextTokens() : nextContextTokens;
    std::atomic_store(&active_, next);
    ++nextGeneration_;
}
void RuntimeGenerations::preflight(config::Catalog catalog) {
    const auto next = std::make_shared<const RuntimeGeneration>(std::move(catalog));
    auto lock = lockOperation();
    Busy busy(busy_);
    const auto old = current();
    if (!old) throw std::runtime_error("No active runtime for context preflight");
    next->executor->reconcile(old->executor->states());
    try {
        for (const auto& [id,context] : next->targetContexts) { (void)id; (void)admitContext(context); }
        model_.useContext(next->defaultPrompt()); model_.useContext(old->defaultPrompt()); ready_ = model_.ready();
    } catch (...) {
        const auto error = std::current_exception();
        try { model_.useContext(old->defaultPrompt()); } catch (...) {}
        ready_ = model_.ready(); std::rethrow_exception(error);
    }
}
std::string RuntimeGenerations::suggest(std::string_view entity, std::string_view name) {
    const auto prompt = suggestionPrompt(entity);
    // Validate input before touching the shared model. The name is data, never a config.
    (void)deterministicId(entity, name);
    auto lock = lockOperation();
    Busy busy(busy_);
    const auto active = current();
    ready_ = model_.ready();
    if (stopping_ || !active || !ready_)
        throw config::ConfigError("ai_unavailable", "", "AI suggestions unavailable. Manual configuration remains available.");
    std::string raw;
    try {
        model_.useContext(prompt);
        raw = model_.infer(name);
        model_.useContext(active->defaultPrompt());
        ready_ = model_.ready();
    } catch (...) {
        const auto error = std::current_exception();
        try { model_.useContext(active->defaultPrompt()); } catch (...) {}
        ready_ = model_.ready();
        std::rethrow_exception(error);
    }
    if (stopping_) throw config::ConfigError("ai_unavailable", "", "Runtime stopping");
    return raw;
}
std::string RuntimeGenerations::draft(std::string_view entity, std::string_view name) {
    (void)deterministicId(entity, name);
    auto lock = lockOperation();
    Busy busy(busy_);
    const auto active = current();
    ready_ = model_.ready();
    if (stopping_ || !active || !ready_) throw config::ConfigError("ai_unavailable", "", "AI suggestions unavailable; manual editing remains available");
    Json proposal = {{"name",name},{"capabilities",Json::array()},{"template","custom"}};
    Json trace = Json::array();
    std::string failed;
    int retries = 1;
    try {
        const std::vector<std::string> groups = entity == "location" ? std::vector<std::string>{"identity","language"} : std::vector<std::string>{"identity","capabilities","language"};
        for (const auto& group : groups) {
            if (entity == "location" && std::string_view(group) == "capabilities") continue;
            if (stopping_) throw std::runtime_error("Runtime stopping");
            auto prompt = draftPrompt(entity, group);
            if (entity == "device" && group == "language") {
                for (const auto& item : Json::parse(templatesJson()))
                    if (item.at("id") == proposal.at("template") && !item.value("noun",std::string{}).empty())
                        prompt += "\nСловарь выбранного пользователем типа: " + item.at("noun").dump() + ". Во всех name/short/synonym используй это полное существительное вместо сокращения; согласуй падежи. Не меняй назначение или аудиторию.";
            }
            const auto decoded = suggestionAttempts(model_,prompt,name,256,
                [&](std::string_view raw){return decodeDraftGroup(raw,entity,group);},trace,retries,stopping_,group);
            if (decoded) proposal.update(*decoded); else failed = "invalid_ai_response";
            if (!failed.empty()) break;
        }
        model_.useContext(active->defaultPrompt()); ready_ = model_.ready();
    } catch (...) {
        const auto error = std::current_exception();
        try { model_.useContext(active->defaultPrompt()); } catch (...) {}
        ready_ = model_.ready(); std::rethrow_exception(error);
    }
    auto result = suggestionReply(trace,failed.empty(),retries);
    if (failed.empty()) result["suggestion"] = proposal;
    return result.dump();
}
std::string RuntimeGenerations::structuredSuggestion(const std::string& prompt, std::string_view input, int budget,
                                                   const SuggestionDecoder& decode) {
    auto lock = lockOperation(); Busy busy(busy_);const auto active = current();
    ready_ = model_.ready();
    if (stopping_ || !active || !ready_) throw config::ConfigError("ai_unavailable","","AI suggestions unavailable; manual editing remains available");
    try {
        Json trace = Json::array(); int retries = 1;
        const auto decoded = suggestionAttempts(model_,prompt,input,budget,decode,trace,retries,stopping_);
        auto result = suggestionReply(trace,decoded.has_value(),retries);
        if (decoded) result["suggestion"] = *decoded;
        model_.useContext(active->defaultPrompt());ready_ = model_.ready();
        return result.dump();
    } catch (...) {
        const auto error = std::current_exception();
        try { model_.useContext(active->defaultPrompt()); } catch (...) {}
        ready_ = model_.ready();std::rethrow_exception(error);
    }
}
RuntimeGenerations::Request::Request(RuntimeGenerations& owner)
    : owner_(owner), lock_(owner.lockOperation()), generation_(owner.current()) {
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
        BoundModel(ContextModel& model, const RuntimeGeneration& generation) : model_(model), generation_(generation) {}
        std::string infer(std::string_view original) override {
            model_.useContext(generation_.requestPrompt(original));
            return model_.infer(original);
        }
    private:
        ContextModel& model_;
        const RuntimeGeneration& generation_;
    } bound(owner_.model_, *generation);
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
