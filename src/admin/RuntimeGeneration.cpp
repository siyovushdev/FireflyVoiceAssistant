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
        retryHint += " Ответ должен содержать ровно один JSON-объект. Не добавляй второй объект или лишние закрывающие скобки. Заверши ответ сразу после закрытия единственного объекта.";
        const auto instructions = prompt + (attempt == 1 ? "" : retryHint);
        Json entry{{"attempt",attempt},{"group",group},{"prompt",instructions},{"input",input},
            {"raw",""},{"metrics",nullptr},{"stage","context_admission"}};
        std::string raw;
        try {
            model.useAdminContext(instructions, budget);
            entry["stage"] = "inference";
            raw = model.setupRequired() ? model.inferAdmin(input) : model.infer(input);
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
        } catch (const Json::parse_error& error) {
            // what() may quote an incomplete UTF-8 fragment of rejected prose.
            // Keep the raw response untouched; serialize only safe parser metadata.
            entry["error"] = "Invalid JSON: parse error " + std::to_string(error.id) +
                " at byte " + std::to_string(error.byte) + "; expected exactly one complete JSON object";
            entry["error_code"] = "invalid_ai_response";
            entry["rejection_stage"] = "json_parse";
            trace.push_back(std::move(entry));
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
std::map<std::string,std::string> contexts(const runtime::DomainView& domain, llm::CommandProtocol protocol) {
    std::map<std::string,std::string> result;
    if (domain.snapshot()->configuration().schemaVersion == 2) {
        for (const auto& device : domain.snapshot()->configuration().devices)
            if (device.kind == config::DeviceKind::Generic) {
                for (const auto& action : device.actionIds)
                    result.emplace(device.id + "/" + action,llm::generateContext(domain,device.id,action).systemPrompt());
            } else if (!device.actionIds.empty()) result.emplace(device.id,llm::generateContext(domain,device.id).systemPrompt());
    }
    if (protocol != llm::CommandProtocol::Structured) {
        for (auto& [key, prompt] : result) {
            const auto slash = key.find('/');
            const auto* target = domain.device(key.substr(0, slash));
            if (target && (slash != std::string::npos || target->actionIds.size() == 1)) {
                llm::SelectedAction selected{target->id, slash == std::string::npos ? target->actionIds.front() : key.substr(slash + 1)};
                if (llm::compactSupported(domain, selected)) prompt = protocol == llm::CommandProtocol::StableCompact ? llm::compactRules() : llm::compactContext(domain, selected);
            }
        }
    }
    return result;
}
}
std::string RuntimeGeneration::defaultPrompt() const {
    if (protocol != llm::CommandProtocol::Structured) return llm::compactRules();
    return targetContexts.empty() ? prompt : R"(Ответ только {"unsupported":true}. Сервер ещё не установил однозначную цель из полного каталога. Не выбирай устройство сам.)";
}
std::string RuntimeGeneration::requestPrompt(std::string_view original) const {
    if (protocol != llm::CommandProtocol::Structured) {
        const auto selected = llm::selectAction(domain, original);
        if (llm::compactSupported(domain, selected))
            return protocol == llm::CommandProtocol::StableCompact ? defaultPrompt() : llm::compactContext(domain, selected);
    }
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
    if (domain.entranceTarget() && detail::lowercase(original).find("двер") != std::string::npos)
        return targetContexts.at(domain.entranceTarget()->id);
    throw config::ConfigError("target_not_grounded", "/text", "No unambiguous configured device mention");
}

RuntimeGeneration::RuntimeGeneration(config::Catalog catalog, std::uint64_t generationId, llm::CommandProtocol mode)
    : id(generationId), protocol(mode), executor(std::make_shared<execution::BoundDeviceExecutor>(catalog)), domain(std::move(catalog)), parser(domain), languageIndex(language::LanguageCompiler::compile(domain.snapshot())), prompt(llm::generateContext(domain).systemPrompt()), targetContexts(contexts(domain, mode)) {
    // Enabling editable deny/disable policy would be a separate consumer change.
    // Do not accept configuration whose policy the current pipeline ignores.
    for (const auto& effective : domain.snapshot()->effectiveActions())
        if (!effective.permitted)
            throw config::ConfigError("unsupported_runtime_policy", "/safety", "This runtime requires enabled devices/actions and no deny restrictions. Deterministic SafetyGate remains mandatory.");
    for (const auto& definition : domain.snapshot()->configuration().actions)
        if (!definition.enabled)
            throw config::ConfigError("unsupported_runtime_policy", "/actions", "Disabled action definitions are not supported by this runtime.");
    for (const auto& target : domain.snapshot()->configuration().devices)
        if (target.kind == config::DeviceKind::EntranceDoor && (!domain.entranceTarget() || target.id != domain.entranceTarget()->id))
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
    RuntimeGeneration admission(catalog, 0, model_.commandProtocol());
    auto lock = lockOperation();
    Busy busy(busy_);
    if (stopping_) throw std::runtime_error("Runtime stopping");
    const auto next = std::make_shared<const RuntimeGeneration>(std::move(catalog), nextGeneration_, model_.commandProtocol());
    if (const auto old = current()) next->executor->reconcile(*old->executor);
    int nextContextTokens = -1;
    try {
        for (const auto& [id,context] : next->targetContexts) { (void)id; nextContextTokens = std::max(nextContextTokens,admitContext(context)); }
        model_.useContext(next->defaultPrompt()); ready_ = !setupBlocked() && model_.ready();
    } catch (...) {
        const auto error = std::current_exception();
        if (const auto old = current()) try { model_.useContext(old->defaultPrompt()); } catch (...) {}
        ready_ = !setupBlocked() && model_.ready(); std::rethrow_exception(error);
    }
    contextTokens_ = next->targetContexts.empty() ? model_.contextTokens() : nextContextTokens;
    std::atomic_store(&active_, next);
    ++nextGeneration_;
}
void RuntimeGenerations::preflight(config::Catalog catalog) {
    const auto next = std::make_shared<const RuntimeGeneration>(std::move(catalog), 0, model_.commandProtocol());
    auto lock = lockOperation();
    Busy busy(busy_);
    const auto old = current();
    if (!old) throw std::runtime_error("No active runtime for context preflight");
    next->executor->reconcile(*old->executor);
    try {
        for (const auto& [id,context] : next->targetContexts) { (void)id; (void)admitContext(context); }
        model_.useContext(next->defaultPrompt()); model_.useContext(old->defaultPrompt()); ready_ = !setupBlocked() && model_.ready();
    } catch (...) {
        const auto error = std::current_exception();
        try { model_.useContext(old->defaultPrompt()); } catch (...) {}
        ready_ = !setupBlocked() && model_.ready(); std::rethrow_exception(error);
    }
}
std::string RuntimeGenerations::suggest(std::string_view entity, std::string_view name) {
    const auto prompt = suggestionPrompt(entity);
    // Validate input before touching the shared model. The name is data, never a config.
    (void)deterministicId(entity, name);
    if (model_.setupRequired() && setupMode() != SetupMode::Active) throw std::runtime_error("setup_required");
    auto lock = lockOperation();
    Busy busy(busy_);
    const auto active = current();
    ready_ = !setupBlocked() && model_.ready();
    if (stopping_ || !active || !model_.ready())
        throw config::ConfigError("ai_unavailable", "", "AI suggestions unavailable. Manual configuration remains available.");
    std::string raw;
    try {
        if(model_.setupRequired()) { model_.useAdminContext(prompt,128); raw=model_.inferAdmin(name); }
        else { model_.useContext(prompt); raw=model_.infer(name); model_.useContext(active->defaultPrompt()); }
        ready_ = !setupBlocked() && model_.ready();
    } catch (...) {
        const auto error = std::current_exception();
        if(!model_.setupRequired()) try { model_.useContext(active->defaultPrompt()); } catch (...) {}
        ready_ = !setupBlocked() && model_.ready();
        std::rethrow_exception(error);
    }
    if (stopping_) throw config::ConfigError("ai_unavailable", "", "Runtime stopping");
    return raw;
}
std::string RuntimeGenerations::draft(std::string_view entity, std::string_view name) {
    (void)deterministicId(entity, name);
    if (model_.setupRequired() && setupMode() != SetupMode::Active) throw std::runtime_error("setup_required");
    auto lock = lockOperation();
    Busy busy(busy_);
    const auto active = current();
    ready_ = !setupBlocked() && model_.ready();
    if (stopping_ || !active || !model_.ready()) throw config::ConfigError("ai_unavailable", "", "AI suggestions unavailable; manual editing remains available");
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
        if(!model_.setupRequired()) model_.useContext(active->defaultPrompt());
        ready_ = !setupBlocked() && model_.ready();
    } catch (...) {
        const auto error = std::current_exception();
        if(!model_.setupRequired()) try { model_.useContext(active->defaultPrompt()); } catch (...) {}
        ready_ = !setupBlocked() && model_.ready(); std::rethrow_exception(error);
    }
    auto result = suggestionReply(trace,failed.empty(),retries);
    if (failed.empty()) result["suggestion"] = proposal;
    return result.dump();
}
const char* RuntimeGenerations::setupModeName() const {
    switch (setupMode()) {
    case SetupMode::Running: return "ASSISTANT_RUNNING";
    case SetupMode::Entering: return "ENTERING_SETUP";
    case SetupMode::Active: return "SETUP_ACTIVE";
    case SetupMode::Restoring: return "RESTORING_ASSISTANT";
    case SetupMode::Degraded: return "RESTART_REQUIRED";
    }
    return "RESTART_REQUIRED";
}
void RuntimeGenerations::enterSetup() {
    auto expected = SetupMode::Running;
    if (!setupMode_.compare_exchange_strong(expected,SetupMode::Entering))
        throw std::runtime_error("setup_busy");
    ++setupEpoch_;
    // Admission closes before waiting for the current lease. No queued commands.
    try {
        if (audioDrain_) audioDrain_();
        auto lock = lockOperation(); Busy busy(busy_);
        ready_ = false;
        model_.enterSetup();
        setupMode_ = SetupMode::Active;
    } catch (...) {
        const auto failure = std::current_exception();
        try { prepareRestore(); finishRestore(); }
        catch (...) { setupMode_ = SetupMode::Degraded; ready_ = false; }
        std::rethrow_exception(failure);
    }
}
void RuntimeGenerations::prepareRestore() {
    auto lock = lockOperation(); Busy busy(busy_);
    setupMode_ = SetupMode::Restoring;
    ready_ = false;
    try {
        const auto active = current();
        if (!active) throw std::runtime_error("No confirmed runtime generation");
        model_.leaveSetup(active->defaultPrompt());
        if (!model_.ready()) throw std::runtime_error("runtime_restore_failed");
        admittedContexts_.clear();
    } catch (...) { setupMode_ = SetupMode::Degraded; throw; }
}
void RuntimeGenerations::finishRestore() {
    auto lock = lockOperation();
    if (setupMode() != SetupMode::Restoring || !model_.ready())
        throw std::runtime_error("runtime_restore_incomplete");
    ready_ = true; setupMode_ = SetupMode::Running;
}
std::string RuntimeGenerations::structuredSuggestion(const std::string& prompt, std::string_view input, int budget,
                                                   const SuggestionDecoder& decode) {
    if (model_.setupRequired() && setupMode() != SetupMode::Active) throw std::runtime_error("setup_required");
    auto lock = lockOperation(); Busy busy(busy_);const auto active = current();
    ready_ = !setupBlocked() && model_.ready();
    if (stopping_ || !active || !model_.ready()) throw config::ConfigError("ai_unavailable","","AI suggestions unavailable; manual editing remains available");
    try {
        Json trace = Json::array(); int retries = 1;
        const auto decoded = suggestionAttempts(model_,prompt,input,budget,decode,trace,retries,stopping_);
        auto result = suggestionReply(trace,decoded.has_value(),retries);
        if (decoded) result["suggestion"] = *decoded;
        if (!model_.setupRequired()) { model_.useContext(active->defaultPrompt());ready_ = !setupBlocked() && model_.ready(); }
        return result.dump();
    } catch (...) {
        const auto error = std::current_exception();
        if (!model_.setupRequired()) try { model_.useContext(active->defaultPrompt()); } catch (...) {}
        ready_ = !setupBlocked() && model_.ready();std::rethrow_exception(error);
    }
}
std::unique_lock<std::timed_mutex> RuntimeGenerations::lockRequest() {
    const auto epoch = setupEpoch_.load();
    if (setupBlocked()) throw std::runtime_error("setup_in_progress");
    auto lock = lockOperation();
    if (setupBlocked() || epoch != setupEpoch_.load()) throw std::runtime_error("setup_in_progress");
    return lock;
}
RuntimeGenerations::Request::Request(RuntimeGenerations& owner)
    : owner_(owner), lock_(owner.lockRequest()), generation_(owner.current()) {
    if (owner.setupBlocked()) throw std::runtime_error("setup_in_progress");
    if (owner.stopping_ || !generation_) throw std::runtime_error("Runtime is not active or is stopping");
    owner_.busy_ = true;
}
RuntimeGenerations::Request::~Request() { owner_.busy_ = false; }
std::unique_ptr<RuntimeGenerations::Request> RuntimeGenerations::beginRequest() {
    if (setupBlocked()) throw std::runtime_error("setup_in_progress");
    return std::unique_ptr<Request>(new Request(*this));
}
GenerationResult RuntimeGenerations::process(std::string_view input, execution::ExecutionIntent execution, std::string source) {
    return beginRequest()->process(input, execution, std::move(source));
}
void RuntimeGenerations::observe(execution::StateObservation observation) {
    auto lock = lockOperation();
    Busy busy(busy_);
    const auto active = current();
    if (!active) throw std::runtime_error("No active runtime");
    if (observation.timestampMs <= 0) observation.timestampMs = active->executor->production().nowMs();
    active->executor->observe(observation);
}
execution::HardwareResult RuntimeGenerations::poll(const std::string& targetId, const std::string& expectedHash) {
    auto lock = lockOperation();
    Busy busy(busy_);
    const auto active = current();
    if (!active || active->domain.snapshot()->contentHash() != expectedHash)
        return {false, "revision_conflict", {}, execution::ReachabilityEvidence::None};
    if (setupBlocked()) return {false, "setup_in_progress", {}, execution::ReachabilityEvidence::None};
    return active->executor->poll(targetId);
}
GenerationResult RuntimeGenerations::Request::process(std::string_view input, execution::ExecutionIntent execution, std::string source, const CommandGuard* guard) {
    if (processed_) throw std::logic_error("Request already consumed");
    processed_ = true;
    const auto& generation = generation_;
    if (guard && generation->domain.snapshot()->contentHash() != guard->expectedHash)
        throw config::ConfigError("revision_conflict", "/expected_hash", "Active configuration changed; refresh before issuing a new command");
    // Bind every fallback to the admitted generation, including recovery after
    // an unsuccessful preflight restoration. Fast-path commands need no model.
    class BoundModel final : public LlmEngine {
    public:
        BoundModel(ContextModel& model, const RuntimeGeneration& generation, bool deterministicOnly)
            : model_(model), generation_(generation), deterministicOnly_(deterministicOnly) {}
        CommandTimings timings;
        std::string infer(std::string_view original) override {
            if (deterministicOnly_) throw config::ConfigError("deterministic_required", "/text", "Operational controls cannot use LLM fallback");
            const auto start = std::chrono::steady_clock::now();
            if (generation_.protocol != llm::CommandProtocol::Structured)
                selected_ = llm::selectAction(generation_.domain, original);
            if (generation_.protocol == llm::CommandProtocol::StableCompact && selected_ &&
                llm::compactSupported(generation_.domain, *selected_)) {
                model_.useContext(generation_.defaultPrompt());
                timings.contextMs = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now() - start).count();
                auto raw = model_.inferCommand(original, llm::compactContract(generation_.domain, *selected_));
                measure(); return raw;
            }
            model_.useContext(selected_ && llm::compactSupported(generation_.domain, *selected_)
                ? llm::compactContext(generation_.domain, *selected_) : generation_.requestPrompt(original));
            timings.contextMs = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now() - start).count();
            auto raw = model_.infer(original); measure(); return raw;
        }
        void measure() {
            const auto value = Json::parse(model_.inferenceMetrics());
            timings.prefillMs = value.value("prefill_ms", 0.0);
            timings.generationMs = value.value("generation_ms", 0.0);
            timings.outputTokens = value.value("output_tokens", 0);
        }
        llm::DecodedResult decode(std::string_view raw) const {
            if (generation_.protocol == llm::CommandProtocol::Structured)
                return llm::StructuredDecoder(generation_.domain).decode(raw);
            if (!selected_) return {actions::Unsupported{}, llm::DecodeStatus::InvalidShape, "No selected contract"};
            return llm::CompactActionDecoder(generation_.domain, *selected_).decode(raw);
        }
    private:
        ContextModel& model_;
        const RuntimeGeneration& generation_;
        const bool deterministicOnly_;
        std::optional<llm::SelectedAction> selected_;
    } bound(owner_.model_, *generation, guard != nullptr);
    CommandEngine engine(bound, generation->parser, generation->domain, [&](std::string_view raw) { return bound.decode(raw); }, generation->languageIndex.get());
    GenerationResult result{generation, engine.process(input), std::nullopt, {}};
    result.command.llmTimings = bound.timings;
    owner_.ready_ = !owner_.setupBlocked() && owner_.model_.ready();
    if (guard) {
        const auto* action = std::get_if<actions::GenericAction>(&result.command.action);
        const auto* door = generation->domain.entranceTarget();
        if (door && guard->expectedAction.targetId() == door->id) result.guardError = "explicit_manual_intent_required";
        else if (result.command.path == CommandPath::LlmFallback) result.guardError = "deterministic_required";
        else if (!action || result.command.safetyRejected || result.command.domainRejected) result.guardError = "command_rejected";
        else if (result.command.path != CommandPath::FastPath || *action != guard->expectedAction) result.guardError = "expected_action_mismatch";
        if (!result.guardError.empty()) return result;
    }
    if (execution != execution::ExecutionIntent::None && !owner_.stopping_) {
        if (const auto* action = std::get_if<actions::GenericAction>(&result.command.action)) {
            // Intent is chosen by the typed call site (voice → Production,
            // /api/test-command → Playground). Source is diagnostic only.
            result.execution = generation->executor->execute(*action, execution);
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
