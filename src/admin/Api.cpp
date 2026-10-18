#include "firefly/SafetyGate.h"
#include "firefly/admin/Api.h"
#include "firefly/admin/AdminTranscriber.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/config/ConfigValidator.h"
#include "firefly/config/Connectivity.h"
#include "firefly/execution/HttpHardwareExecutor.h"
#include "firefly/execution/MqttConnectionProbe.h"
#include "Json.h"
#include "SetupSession.h"
#include "firefly/admin/AdminDraft.h"
#include "firefly/admin/AdminCopilot.h"
#include <atomic>
#include <iostream>
#include <mutex>

namespace firefly::admin {
namespace {
struct ApiError : std::runtime_error {
    int status;
    std::string code, path;
    ApiError(int status, std::string code, std::string path, std::string message)
        : std::runtime_error(std::move(message)), status(status), code(std::move(code)), path(std::move(path)) {}
};
Json describe(const config::Catalog& catalog) {
    return {{"configuration", Json::parse(catalog->canonicalJson())}, {"revision", catalog->configuration().revision}, {"hash", catalog->contentHash()}};
}
config::ExpectedRevision expected(const Json& root) {
    const auto& value = root.at("expected");
    members(value, {"revision", "hash"});
    if (!value["revision"].is_number_integer() || !value["hash"].is_string()) throw std::invalid_argument("Invalid expected identity");
    const auto revision = value["revision"].get<std::int64_t>();
    const auto hash = value["hash"].get<std::string>();
    if (revision < 1 || revision > config::MaxRevision || hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("Invalid expected revision/hash");
    return {revision, hash};
}
void checkExpected(const config::Catalog& stored, const config::ExpectedRevision& value) {
    if (stored->configuration().revision != value.revision || stored->contentHash() != value.hash)
        throw config::ConfigError("revision_conflict", "", "Stored revision changed; reload before saving or applying.");
}
std::shared_ptr<const RuntimeGeneration> candidate(const Json& root) {
    auto catalog = config::ConfigLoader::fromJson(root.at("configuration").dump());
    try { return std::make_shared<const RuntimeGeneration>(std::move(catalog)); }
    catch (const config::ConfigError&) { throw; }
    catch (const std::exception& error) {
        std::cerr << "Admin generation validation: " << error.what() << '\n';
        throw ApiError(422, "invalid_runtime_domain", "", "Domain is ambiguous or exceeds the LLM context resource limit.");
    }
}
}
struct Api::Impl {
    config::ConfigStore& store;
    RuntimeClient& runtime;
    execution::MqttCredentials secrets;
    std::shared_ptr<LocalMqttBrokerControl> brokerControl;
    std::mutex brokerControlOperation;
    AdminTranscriber* transcriber = nullptr;
    HealthModel health;
    std::mutex writes, runtimeOperation, state;
    std::atomic<bool> busy{false};
    std::unique_ptr<SetupSession> setup;
    std::atomic<bool> timerStop{false};
    std::thread timer;
    Json active = nullptr;
    bool ready = false;
    int workerPid = 0;
    std::string lastError;
    Impl(config::ConfigStore& store, RuntimeClient& runtime, AdminTranscriber* transcriber,
         std::string credentialsDirectory,
         std::shared_ptr<LocalMqttBrokerControl> control)
        : store(store), runtime(runtime), secrets(std::move(credentialsDirectory)),
          brokerControl(control ? std::move(control) : std::make_shared<LocalMqttBrokerControl>()),
          transcriber(transcriber) {
        setup = std::make_unique<SetupSession>(
            store,
            [this](const Json& value) { return invoke(value); },
            [this](const config::Catalog& candidate) {
                std::unique_lock<std::mutex> mutation(writes);
                secrets.verifyReferences(candidate);
                return mutation;
            },
            [this] {
                // The same gate is held by broker-control operations.
                // Setup cannot publish active=true in their critical section.
                return std::unique_lock<std::mutex>(brokerControlOperation);
            });
        timer = std::thread([this] { while (!timerStop) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!timerStop) try { setup->tick(); } catch (const std::exception&) { /* explicit setup status remains unavailable */ }
        }});
    }
    ~Impl() { timerStop = true; if (timer.joinable()) timer.join(); setup.reset(); }
    void syncHealthFromRuntime(const Json& live) {
        const bool modelReady = live.value("model_ready", false);
        if (modelReady) health.setSubsystem("llm", SubsystemStatus::Ready);
        else if (live.contains("active") && !live.at("active").is_null())
            health.setSubsystem("llm", SubsystemStatus::Failed, "model_unavailable",
                live.value("runtime_error", std::string("Model not ready")));
        else health.setSubsystem("llm", SubsystemStatus::Starting);
        if (live.contains("active") && !live.at("active").is_null()) {
            health.setSubsystem("runtime", SubsystemStatus::Ready);
            health.setSubsystem("hardware_execution", SubsystemStatus::Ready);
        }
        if (transcriber && transcriber->ready())
            health.setSubsystem("stt", SubsystemStatus::Ready);
        else if (transcriber)
            health.setSubsystem("stt", SubsystemStatus::Failed, "stt_unavailable");
        else health.setSubsystem("stt", SubsystemStatus::Unavailable, "", "not_configured");
        if (live.value("setup_mode",std::string("ASSISTANT_RUNNING")) != "ASSISTANT_RUNNING") {
            health.setSubsystem("runtime",SubsystemStatus::Unavailable,"setup_in_progress",live.value("setup_mode",std::string()));
            health.setSubsystem("hardware_execution",SubsystemStatus::Unavailable,"setup_in_progress");
            const auto mode=live.value("setup_mode",std::string());
            health.setSubsystem("setup",mode=="RESTART_REQUIRED"?SubsystemStatus::Failed:SubsystemStatus::Ready,
                mode=="RESTART_REQUIRED"?"restart_required":"",mode);
        } else {
            health.setSubsystem("setup",SubsystemStatus::Ready,{},"ASSISTANT_RUNNING");
        }
        health.refreshOverall();
    }
    Json invoke(const Json& request) {
        std::unique_lock<std::mutex> operation(runtimeOperation, std::defer_lock);
        const bool entering = request.value("method",std::string())=="setup_enter";
        // The local runtime must close voice admission immediately, before the
        // API mutex of an accepted command is released. Its generation lock then
        // waits for that command; this does not permit concurrent inference.
        const bool localEntry = entering && runtime.concurrentSetupEntry();
        if (entering && !localEntry) operation.lock();
        else if (localEntry) { /* RuntimeGenerations owns the admission barrier. */ }
        else operation.try_lock();
        if (!localEntry && !operation.owns_lock()) throw ApiError(409, "runtime_busy", "", "Runtime is processing another command or Apply. Retry when idle.");
        if (health.snapshot().stopping)
            throw ApiError(503, "runtime_stopping", "", "Runtime is shutting down.");
        struct Busy { std::atomic<bool>& flag; explicit Busy(std::atomic<bool>& f) : flag(f) { flag = true; } ~Busy() { flag = false; } } guard(busy);
        Json reply;
        try {
            reply = parseMessage(runtime.call(request.dump()));
            if (reply.value("protocol_version", 0) != 1 || !reply.contains("active")) throw std::runtime_error("Runtime IPC version mismatch");
        } catch (const std::exception& error) {
            std::cerr << "Admin runtime IPC: " << error.what() << '\n';
            std::lock_guard<std::mutex> lock(state);
            ready = false; lastError = "Inference worker unavailable. Configuration editing remains available.";
            health.setSubsystem("llm", SubsystemStatus::Failed, "runtime_unavailable", lastError);
            throw ApiError(503, "runtime_unavailable", "", lastError);
        }
        {
            std::lock_guard<std::mutex> lock(state);
            active = reply.at("active");
            ready = reply.value("model_ready", false);
            workerPid = reply.value("worker_pid", 0);
            lastError = reply.value("ok", false) ? "" : "Runtime rejected the request; active identity is shown separately.";
        }
        return reply;
    }
    Json validate(const Json& root, bool preflight) {
        const auto generation = candidate(root);
        secrets.verifyReferences(generation->domain.snapshot());
        Json result{{"ok", true}, {"valid", true}, {"context_bytes", generation->prompt.size()}, {"context", generation->prompt}, {"runtime_preflight", "unavailable"}};
        bool modelReady;
        { std::lock_guard<std::mutex> lock(state); modelReady = ready; }
        if (preflight && modelReady) {
            auto checked = invoke({{"method", "preflight"}, {"configuration", Json::parse(generation->domain.snapshot()->canonicalJson())}});
            if (!checked.value("ok", false)) {
                checked["valid"] = false;
                return checked;
            }
            result["runtime_preflight"] = "verified";
        }
        result["language_conflicts"] = Json::array();
        for (const auto& c : generation->languageIndex->conflicts()) result["language_conflicts"].push_back({{"severity",c.severity},{"code",c.code},{"alias",c.alias},{"entities",c.entities}});
        result["language_index"] = {{"fingerprint",generation->languageIndex->fingerprint()},{"compiler",language::LanguageIndex::CompilerVersion}};
        result["language_examples"] = Json::array();
        const auto& domain = generation->domain;
        for (const auto& device : domain.snapshot()->configuration().devices) {
            if (device.spokenAliases.empty()) continue;
            for (const auto& id : device.actionIds) {
                if (result["language_examples"].size() == 5) break;
                const auto& action = domain.snapshot()->configuration().actions.at(domain.snapshot()->actionIndex().at(id));
                if (action.aliases.empty() || action.parameters.size() > 1) continue;
                std::string example = action.aliases.front() + " " + device.spokenAliases.front();
                if (device.roomId) {
                    const auto* room = domain.room(*device.roomId);
                    if (room && !room->locations.empty()) example += " " + room->locations.front();
                }
                if (!action.parameters.empty()) {
                    const auto& parameter = action.parameters.front();
                    if (parameter.type == "integer") example += " " + std::to_string(parameter.minimum);
                    else if (parameter.type == "boolean") example += " true";
                    else if (parameter.type == "enum" && !parameter.values.empty()) example += " " + parameter.values.front();
                    else continue;
                }
                const auto matched = generation->languageIndex->match(example);
                const auto safe = SafetyGate::apply(example, matched.candidate, domain);
                if (std::holds_alternative<actions::GenericAction>(domain.validate(safe))) result["language_examples"].push_back(example);
            }
        }
        result["apply_ready"] = result["runtime_preflight"] == "verified";
        return result;
    }
};
Api::Api(config::ConfigStore& store, RuntimeClient& runtime, AdminTranscriber* transcriber,
         std::string mqttCredentialsDirectory,
         std::shared_ptr<LocalMqttBrokerControl> mqttBrokerControl)
    : impl_(std::make_unique<Impl>(store, runtime, transcriber,
                                   std::move(mqttCredentialsDirectory),
                                   std::move(mqttBrokerControl))) {}
Api::~Api() = default;
HealthModel& Api::health() { return impl_->health; }
const HealthModel& Api::health() const { return impl_->health; }
void Api::noteAdminListening() {
    impl_->health.setSubsystem("admin", SubsystemStatus::Ready);
    impl_->health.setPhase("http_listening");
    std::cerr << "STARTUP admin HTTP listening\n";
}
void Api::noteStt(bool ready, std::string detail) {
    if (ready) impl_->health.setSubsystem("stt", SubsystemStatus::Ready);
    else impl_->health.setSubsystem("stt",
        detail.empty() ? SubsystemStatus::Unavailable : SubsystemStatus::Failed,
        detail.empty() ? "" : "stt_unavailable", detail);
}
void Api::attachTranscriber(AdminTranscriber* transcriber) {
    impl_->transcriber = transcriber;
}
void Api::beginShutdown() {
    impl_->timerStop = true;
    std::cerr << "SHUTDOWN requested\n";
    impl_->health.setStopping(true);
}
void Api::initializeRuntime() {
    // Serialize boot-time credential admission, runtime publication and LKG
    // selection against concurrent Admin credential DELETE / Save / Apply.
    // Otherwise a checked secret could disappear before being used.
    std::lock_guard<std::mutex> mutation(impl_->writes);
    impl_->health.setPhase("configuration_loading");
    std::cerr << "STARTUP configuration loading\n";
    try {
        try { (void)impl_->store.reconcile(); }
        catch (const config::ConfigError& error) {
            std::cerr << "STARTUP store reconcile: " << error.code << " " << error.what() << '\n';
        }

        config::Catalog catalog;
        bool recovered = false;
        std::string configError;
        try {
            // A Catalog only becomes selected AFTER all admission checks.
            // A failed secret check must not leave a truthy rejected candidate
            // for the later 'if (!catalog)' gate.
            auto checkedCurrent = impl_->store.current();
            impl_->secrets.verifyReferences(checkedCurrent);
            (void)std::make_shared<const RuntimeGeneration>(checkedCurrent);
            catalog = std::move(checkedCurrent);
            impl_->health.setSubsystem("configuration", SubsystemStatus::Ready);
        } catch (const std::exception& error) {
            configError = error.what();
            std::cerr << "STARTUP current config unusable: " << configError << '\n';
            try {
                auto lkg = impl_->store.lastKnownGood();
                if (lkg) {
                    // LKG is an independent admission candidate, not a bypass.
                    // It may itself reference a deleted/malformed secret.
                    impl_->secrets.verifyReferences(*lkg);
                    (void)std::make_shared<const RuntimeGeneration>(*lkg);
                    catalog = *lkg;
                    recovered = true;
                    impl_->health.setConfigRecovered(true);
                    impl_->health.setSubsystem("configuration", SubsystemStatus::Ready, "config_recovered",
                        "Active store invalid; runtime using Last Known Good");
                    std::cerr << "STARTUP config recovered from Last Known Good revision="
                              << catalog->configuration().revision << '\n';
                }
            } catch (const std::exception& lkgError) {
                std::cerr << "STARTUP Last Known Good unusable: " << lkgError.what() << '\n';
            }
        }

        if (!catalog) {
            impl_->health.setSubsystem("configuration", SubsystemStatus::Failed, "config_unusable", configError);
            impl_->health.setSubsystem("runtime", SubsystemStatus::Failed, "no_active_generation");
            impl_->health.setSubsystem("hardware_execution", SubsystemStatus::Unavailable, "config_unusable");
            impl_->health.setSubsystem("llm", SubsystemStatus::Unavailable, "config_unusable");
            impl_->health.setPhase("recovery_mode");
            impl_->health.refreshOverall();
            std::cerr << "STARTUP recovery mode: Admin available, execution disabled\n";
            return;
        }

        impl_->health.setPhase("runtime_initializing");
        std::cerr << "STARTUP runtime/domain initializing\n";
        try {
            auto reply = impl_->invoke({{"method", "apply"}, {"configuration", Json::parse(catalog->canonicalJson())}});
            if (!reply.value("ok", false)) {
                impl_->health.setSubsystem("runtime", SubsystemStatus::Failed, "apply_rejected");
                impl_->health.setSubsystem("llm", SubsystemStatus::Failed, "apply_rejected");
                impl_->health.setSubsystem("hardware_execution", SubsystemStatus::Unavailable);
                impl_->health.setPhase("degraded_startup");
                impl_->health.refreshOverall();
                std::cerr << "STARTUP apply rejected; Admin remains available\n";
                return;
            }
            if (!recovered) {
                try { impl_->store.recordLastKnownGood(catalog); }
                catch (const std::exception& error) {
                    std::cerr << "STARTUP LKG record failed: " << error.what() << '\n';
                }
            }
            impl_->health.setSubsystem("runtime", SubsystemStatus::Ready);
            impl_->health.setSubsystem("hardware_execution", SubsystemStatus::Ready);
            if (reply.value("model_ready", false))
                impl_->health.setSubsystem("llm", SubsystemStatus::Ready);
            else
                impl_->health.setSubsystem("llm", SubsystemStatus::Failed, "model_unavailable",
                    "Runtime admitted without ready model");
            impl_->health.setPhase(recovered ? "degraded_startup" : "ready");
            std::cerr << "STARTUP runtime ready recovered=" << (recovered ? "yes" : "no")
                      << " model_ready=" << reply.value("model_ready", false) << '\n';
        } catch (const std::exception& error) {
            std::cerr << "STARTUP runtime apply: " << error.what() << '\n';
            impl_->health.setSubsystem("runtime", SubsystemStatus::Failed, "runtime_unavailable", error.what());
            impl_->health.setSubsystem("llm", SubsystemStatus::Failed, "runtime_unavailable");
            impl_->health.setSubsystem("hardware_execution", SubsystemStatus::Unavailable);
            impl_->health.setPhase("degraded_startup");
        }
    } catch (const std::exception& error) {
        std::cerr << "STARTUP fatal: " << error.what() << '\n';
        impl_->health.setSubsystem("configuration", SubsystemStatus::Failed, "startup_fatal", error.what());
        impl_->health.setPhase("failed");
    }
    impl_->health.refreshOverall();
}
HttpReply Api::handle(std::string_view method, std::string_view path, std::string_view body, std::string_view setupOwner) {
    int status = 200;
    Json reply;
    try {
        if (path.substr(0,11) == "/api/setup/") {
            const auto route=path.substr(11);
            if ((route=="status" && method!="GET") || (route!="status" && method!="POST"))
                throw ApiError(405,"method_not_allowed","","Unsupported setup method");
            try {
                const auto result=impl_->setup->handle(route,method=="GET"?Json::object():parseMessage(body,true));
                return {200,"application/json; charset=utf-8",result.dump()};
            } catch (const std::exception& error) {
                return {409,"application/json; charset=utf-8",failure("setup_failed","",error.what()).dump()};
            }
        }
        const std::map<std::string, std::string> routes{{"/api/admin/language-profile", "POST"}, {"/api/admin/plan", "POST"}, {"/api/admin/copilot", "POST"}, {"/api/admin/copilot/accept", "POST"}, {"/api/admin/transcribe", "POST"}, {"/api/admin/action-draft", "POST"}, {"/api/admin/templates", "GET"}, {"/api/admin/draft", "POST"}, {"/api/config/migrate", "POST"}, {"/api/admin/id", "POST"}, {"/api/admin/suggest", "POST"}, {"/api/config/diff", "POST"},
            {"/api/config/export", "GET"}, {"/api/config/import", "POST"}, {"/api/config/revision", "POST"},
            {"/api/config/restore-candidate", "POST"}, {"/api/dashboard", "GET"}, {"/api/devices/state", "GET"},
            {"/api/connections/test", "POST"}, {"/api/connections/probe", "POST"}, {"/api/mqtt/credentials", "GET POST PUT DELETE"}, {"/api/mqtt/local-broker", "GET POST"}, {"/api/devices/command", "POST"}, {"/api/devices/poll", "POST"},
            {"/api/executions", "GET"}, {"/api/status", "GET"}, {"/api/config", "GET PUT"},
            {"/api/config/active", "GET"}, {"/api/config/validate", "POST"}, {"/api/config/apply", "POST"},
            {"/api/config/revisions", "GET"}, {"/api/config/restore", "POST"}, {"/api/config/reconcile", "POST"}, {"/api/test-command", "POST"}};
        const auto route = routes.find(std::string(path));
        if (route == routes.end()) throw ApiError(404, "not_found", "", "Unknown API endpoint.");
        if ((" " + route->second + " ").find(" " + std::string(method) + " ") == std::string::npos)
            throw ApiError(405, "method_not_allowed", "", "HTTP method is not supported for this endpoint.");
        if (method == "GET" && !body.empty()) throw std::invalid_argument("Unexpected body");
        if (path == "/api/admin/transcribe") {
            if (impl_->setup->active()) impl_->setup->handle("authorize",Json{{"session_id",setupOwner}});
            if (!impl_->transcriber || !impl_->transcriber->ready())
                throw ApiError(503, "stt_unavailable", "", "Local speech recognition is not available on this Firefly.");
            reply = Json::parse(impl_->transcriber->transcribe(body, impl_->setup->active() ? 60 : 15));
            if (!reply.value("ok", false)) status = reply.contains("errors") &&
                reply["errors"].is_array() && !reply["errors"].empty() &&
                reply["errors"][0].value("code", "") == "stt_busy" ? 409 : 422;
            return {status, "application/json; charset=utf-8", reply.dump(-1, ' ', false, Json::error_handler_t::replace)};
        }
        Json request = method == "GET" ? Json::object() : parseMessage(body,true);
        if (impl_->setup->active() && method != "GET") {
            if (path == "/api/config" || path == "/api/config/apply" || path == "/api/config/restore" || path == "/api/config/reconcile" || path == "/api/test-command" || path == "/api/devices/command" ||
                path == "/api/mqtt/local-broker")
                throw ApiError(409,"setup_in_progress","","Настройка активна; используйте её подтверждение или отмену.");
            if (path == "/api/connections/probe") {
                try { impl_->setup->handle("authorize", request); }
                catch (const std::exception&) {
                    throw ApiError(409, "setup_authorization_failed", "/session_id", "The active setup owner must authorize this probe.");
                }
            } else impl_->setup->handle("authorize",request);
            request.erase("session_id");
        }
        const auto observation = method == "GET" ? impl_->runtime.observe() : std::string{};
        if (path == "/api/admin/plan" || path == "/api/admin/action-draft" || path == "/api/admin/copilot") {
            if (path == "/api/admin/plan") { members(request,{"text","configuration"}); request["method"] = "plan"; }
            else if (path == "/api/admin/copilot") { members(request,{"text","configuration"}); request["method"] = "copilot"; }
            else { members(request,{"name","target","configuration"}); request["method"] = "action_draft"; }
            (void)config::ConfigLoader::fromJson(request.at("configuration").dump());
            reply = impl_->invoke(request);if(!reply.value("ok",false))status=422;
        } else if (path == "/api/admin/copilot/accept") {
            members(request,{"configuration","proposal"});
            reply = Json::parse(acceptCopilotProposal(request.at("configuration").dump(), request.at("proposal").dump()));
            reply["ok"] = true; status = 200;
        } else if (path == "/api/admin/templates") {
            reply = {{"ok",true},{"templates",Json::parse(templatesJson())}};
        } else if (path == "/api/config/migrate") {
            members(request, {"configuration"});
            const auto old = config::ConfigLoader::fromJson(request.at("configuration").dump());
            const auto migrated = config::ConfigValidator::validate(config::migrateV2(old->configuration()));
            reply = describe(migrated); reply["ok"] = true; reply["saved"] = false; reply["applied"] = false;
            reply["from_version"] = old->configuration().schemaVersion;
        } else if (path == "/api/admin/draft") {
            members(request, {"entity","name"});
            (void)deterministicId(request.at("entity").get<std::string>(),request.at("name").get<std::string>());
            request["method"] = "draft"; reply = impl_->invoke(request);
            if (!reply.value("ok",false)) status = 422;
        } else if (path == "/api/admin/id") {
            members(request, {"category","name","occupied"});
            const auto occupied = request.at("occupied").get<std::set<std::string>>();
            if (occupied.size() > 8192) throw std::invalid_argument("Too many IDs");
            reply = {{"ok",true},{"id",deterministicId(request.at("category").get<std::string>(),request.at("name").get<std::string>(),occupied)},{"algorithm","transliteration_v1"}};
        } else if (path == "/api/admin/language-profile") {
            members(request, {"configuration", "entity", "id"});
            request["method"] = "language_profile"; reply = impl_->invoke(request);
            if (!reply.value("ok", false)) status = 422;
        } else if (path == "/api/admin/suggest") {
            members(request, {"entity","name"});
            (void)deterministicId(request.at("entity").get<std::string>(),request.at("name").get<std::string>());
            request["method"] = "suggest"; reply = impl_->invoke(request);
            if (!reply.value("ok",false)) status = 422;
        } else if (path == "/api/config/export") {
            reply = Json::parse(impl_->store.current()->canonicalJson());
        } else if (path == "/api/config/import") {
            members(request, {"document"});
            const auto imported = config::ConfigLoader::fromJson(request.at("document").get<std::string>());
            impl_->secrets.verifyReferences(imported);
            RuntimeGeneration admission(imported);
            reply = describe(imported);
            reply["ok"] = true; reply["saved"] = false; reply["applied"] = false;
        } else if (path == "/api/config/diff") {
            members(request, {"configuration","base"});
            const auto next = config::ConfigLoader::fromJson(request.at("configuration").dump());
            Json previous;
            if (request.at("base") == "stored") previous = Json::parse(impl_->store.current()->canonicalJson());
            else if (request.at("base") == "active") {
                const auto live = impl_->runtime.observe();
                std::lock_guard<std::mutex> lock(impl_->state);
                const auto active = live.empty() ? impl_->active : parseMessage(live).at("active");
                if (active.is_null()) {
                    // Initial activation/recovery must remain accessible when no
                    // model generation has ever been admitted. Show all entities.
                    previous = Json::parse(next->canonicalJson());
                    for (const auto* key : {"rooms","devices","actions","bindings","connections","observations"}) previous[key] = Json::array();
                    for (const auto* key : {"system","safety","compatibility"}) previous[key] = nullptr;
                } else previous = active.at("configuration");
            } else throw std::invalid_argument("Unknown diff base");
            reply = {{"ok",true},{"changes",Json::parse(configurationDiff(previous.dump(),next->canonicalJson()))}};
            if (request.at("base") == "active") {
                const auto state = impl_->invoke({{"method","state_diff"},{"configuration",Json::parse(next->canonicalJson())}});
                if (state.value("ok",false)) { reply["state_reconciliation"] = state;for(const auto& change:state.at("changes"))reply["changes"].push_back(change); }
            }
        } else if (path == "/api/config/revision" || path == "/api/config/restore-candidate") {
            const bool restore = path == "/api/config/restore-candidate";
            members(request, restore ? std::set<std::string>{"revision","expected"} : std::set<std::string>{"revision"});
            if (!request.at("revision").is_number_integer()) throw std::invalid_argument("Integer revision required");
            const auto number = request.at("revision").get<std::int64_t>();
            reply = describe(restore ? impl_->store.restoreCandidate(number,expected(request)) : impl_->store.revision(number));
            reply["ok"] = true; reply["saved"] = false; reply["applied"] = false;
        } else if (path == "/api/dashboard") {
            const auto live = observation.empty() ? impl_->invoke({{"method","observe"}}) : parseMessage(observation);
            const auto stored = impl_->store.current();
            const bool pending = live["active"].is_null() || live["active"]["hash"] != stored->contentHash();
            reply = {{"ok",true},{"simple",{{"ready",live["model_ready"]},{"busy",live.value("runtime_busy",false)},
                {"voice",live.value("voice","disabled")},{"executor","mock"},{"devices",live["devices"].size()},
                {"pending_changes",pending}}},{"advanced",live}};
            reply["advanced"]["stored_revision"] = stored->configuration().revision;
        } else if (path == "/api/devices/state" || path == "/api/executions") {
            const auto live = observation.empty() ? impl_->invoke({{"method","observe"}}) : parseMessage(observation);
            const auto key = path == "/api/devices/state" ? "devices" : "executions";
            reply = {{"ok",true},{key,live.at(key)},{"active",live.at("active")}};
            if (path == "/api/devices/state" && live.contains("playground_devices"))
                reply["playground_devices"] = live.at("playground_devices");
        } else if (path == "/api/mqtt/local-broker") {
            // Broker enable/disable is operational, never part of config
            // Save/Apply. The trusted Unix-socket helper owns the fixed unit
            // and must verify actual systemd status before acknowledging.
            std::lock_guard<std::mutex> control(impl_->brokerControlOperation);
            // The pre-dispatch check is advisory only: Setup may have entered
            // after that check but before acquiring brokerControlOperation.
            // The SetupSession entry gate serializes active_=true with this
            // recheck. No privileged mutation may pass it while Setup is active.
            if (method == "POST" && impl_->setup->active())
                throw ApiError(409, "setup_in_progress", "",
                               "Настройка активна; изменение локального брокера запрещено.");
            LocalMqttBrokerStatus state;
            if (method == "GET") {
                state = impl_->brokerControl->status();
            } else {
                members(request, {"operation", "confirm"});
                if (!request.at("confirm").is_boolean() ||
                    !request.at("confirm").get<bool>())
                    throw ApiError(422, "mqtt_broker_confirmation_required", "/confirm",
                                   "Explicit confirmation is required.");
                const auto operation = request.at("operation").get<std::string>();
                if (operation == "enable") state = impl_->brokerControl->enable();
                else if (operation == "disable") state = impl_->brokerControl->disable();
                else throw ApiError(422, "mqtt_broker_invalid_operation", "/operation",
                                    "Only enable or disable are allowed.");
            }
            reply = {{"ok", true}, {"installed", state.installed},
                     {"enabled", state.enabled}, {"running", state.running},
                     {"scope", "local_mosquitto_only"}};
        } else if (path == "/api/mqtt/credentials") {
            // Browser/API operations use the existing loopback-only Admin
            // Host/Origin/custom-header policy. Do not echo secret contents,
            // username or password to the browser after write, even on error.
            if (method == "GET") {
                reply = {{"ok", true}, {"ids", impl_->secrets.ids()}};
            } else {
                if (method == "POST" || method == "PUT") {
                    members(request, {"id", "username", "password"});
                    const auto id = request.at("id").get<std::string>();
                    const auto username = request.at("username").get<std::string>();
                    const auto password = request.at("password").get<std::string>();
                    std::lock_guard<std::mutex> mutation(impl_->writes);
                    if (method == "POST") impl_->secrets.create(id, username, password);
                    else impl_->secrets.rotate(id, username, password);
                    reply = {{"ok", true}, {"id", id},
                             {"operation", method == "POST" ? "created" : "rotated"},
                             {"requires_apply", true}};
                } else {
                    members(request, {"id"});
                    const auto id = request.at("id").get<std::string>();
                    std::lock_guard<std::mutex> mutation(impl_->writes);
                    const auto inUse = [&](const config::Catalog& catalog) {
                        if (!catalog) return false;
                        for (const auto& c : catalog->configuration().connections)
                            if (c.type == config::ExecutorKind::Mqtt &&
                                c.mqtt.credentialsId == id) return true;
                        return false;
                    };
                    // Protect not only stored/active graphs but retained
                    // history and LKG: a future restore must remain valid.
                    if (inUse(impl_->store.current()))
                        throw ApiError(409, "mqtt_credentials_in_use", "/id",
                                       "Remove credential references from configuration first.");
                    for (const auto& history : impl_->store.revisions())
                        if (inUse(impl_->store.revision(history.revision)))
                            throw ApiError(409, "mqtt_credentials_in_use", "/id",
                                           "A retained configuration revision still uses this credential.");
                    if (const auto lkg = impl_->store.lastKnownGood())
                        if (inUse(*lkg))
                            throw ApiError(409, "mqtt_credentials_in_use", "/id",
                                           "Last Known Good still uses this credential.");
                    Json active;
                    {
                        std::lock_guard<std::mutex> lock(impl_->state);
                        active = impl_->active;
                    }
                    if (active.is_object() && active.contains("configuration") &&
                        inUse(config::ConfigLoader::fromJson(active.at("configuration").dump())))
                        throw ApiError(409, "mqtt_credentials_in_use", "/id",
                                       "The active runtime still uses this credential.");
                    impl_->secrets.erase(id);
                    reply = {{"ok", true}, {"id", id}, {"operation", "deleted"}};
                }
            }
        } else if (path == "/api/connections/probe") {
            members(request, {"connection"});
            const auto connection = config::ConfigLoader::connectionFromJson(request.at("connection").dump());
            if (connection.type == config::ExecutorKind::Mqtt) {
                // A fresh, bounded client verifies MQTT CONNECT/CONNACK with
                // its private credentials and CA/hostname TLS trust. The
                // active executor, broker service and ConfigStore are untouched.
                // In particular: zero PUBLISH, SUBSCRIBE, Save or Apply.
                const auto result = execution::MqttConnectionProbe::testConnection(
                    connection, impl_->secrets);
                if (result.success) {
                    reply = {{"ok",true},{"scope","mqtt_connack_only"},
                             {"transport","mqtt"},{"connack_accepted",true},
                             {"tls_verified",result.tlsVerified},
                             {"credentials_accepted",result.credentialsAccepted},
                             {"saved",false},{"applied",false},
                             {"published",false},{"subscribed",false}};
                } else {
                    status = result.code == "mqtt_probe_busy" ? 409 : 422;
                    reply = failure(result.code, "/connection",
                        "MQTT CONNECT was unavailable, rejected or not verified. No MQTT commands or subscriptions were sent.");
                }
            } else {
                const auto result = execution::HttpHardwareExecutor::testConnection(connection);
                reply = result.success ? Json{{"ok",true},{"scope","tcp_connect_only"},{"saved",false},{"applied",false}}
                    : failure(result.error, "/connection", "TCP reachability test failed. No HTTP bytes were sent.");
                if (!result.success) status = 422;
            }
        } else if (path == "/api/devices/command") {
            members(request, {"text", "expected_hash", "expected_action"});
            request["method"] = "guarded_command";
            reply = impl_->invoke(request);
            if (!reply.value("ok", false)) status = 422;
        } else if (path == "/api/connections/test") {
            members(request, {"configuration", "connection_id"});
            const auto draft = config::ConfigLoader::fromJson(request.at("configuration").dump());
            const auto* connection = config::findConnection(draft->configuration(), request.at("connection_id"));
            if (!connection) throw config::ConfigError("dangling_reference", "/connection_id", "Unknown connection");
            const auto result = execution::HttpHardwareExecutor::testConnection(*connection);
            reply = result.success ? Json{{"ok",true},{"scope","tcp_connect_only"},{"saved",false},{"applied",false}}
                : failure(result.error, "", "TCP reachability test failed. No HTTP command was sent.");
            if (!result.success) status = 422;
        } else if (path == "/api/devices/poll") {
            members(request, {"target_id", "expected_hash"});
            reply = impl_->invoke({{"method","poll_state"},{"target_id",request.at("target_id")},{"expected_hash",request.at("expected_hash")}});
            if (!reply.value("ok", false)) status = 422;
        } else if (path == "/api/status") {
            reply = {{"ok", true}, {"runtime_busy", impl_->busy.load()}};
            try { const auto stored = impl_->store.current(); reply["stored"] = {{"revision", stored->configuration().revision}, {"hash", stored->contentHash()}}; }
            catch (const config::ConfigError& error) { reply["store_error"] = error.code; }
            std::lock_guard<std::mutex> lock(impl_->state);
            reply["runtime_ready"] = impl_->ready; reply["worker_pid"] = impl_->workerPid; reply["runtime_error"] = impl_->lastError;
            reply["active"] = impl_->active.is_null() ? Json(nullptr) : Json{{"revision", impl_->active["revision"]}, {"hash", impl_->active["hash"]}};
            if (!observation.empty()) {
                const auto live = parseMessage(observation);
                reply["runtime_ready"] = live["model_ready"]; reply["runtime_busy"] = live["runtime_busy"];
                reply["worker_pid"] = live["worker_pid"];
                reply["active"] = live["active"].is_null() ? Json(nullptr) : Json{{"revision",live["active"]["revision"]},
                    {"hash",live["active"]["hash"]},{"generation_id",live["active"]["generation_id"]}};
                for (const auto* key : {"voice","microphone","executor","last_command","last_voice","uptime_seconds","context_tokens","context_limit","build"}) reply[key] = live[key];
                reply["device_count"] = live["devices"].size();
                impl_->syncHealthFromRuntime(live);
            }
            reply["needs_apply"] = !reply.contains("stored") || reply["active"].is_null() || reply["stored"]["hash"] != reply["active"]["hash"];
            reply["stt_ready"] = impl_->transcriber != nullptr && impl_->transcriber->ready();
            const auto health = Json::parse(HealthModel::toJson(impl_->health.snapshot()));
            reply["health"] = health;
            reply["startup_phase"] = health.at("phase");
            reply["config_recovered"] = health.at("config_recovered");
            reply["stopping"] = health.at("stopping");
        } else if (path == "/api/config/active") {
            std::lock_guard<std::mutex> lock(impl_->state);
            reply = {{"ok", true}, {"active", observation.empty() ? impl_->active : parseMessage(observation).at("active")}};
        } else if (path == "/api/config" && method == "GET") { reply = describe(impl_->store.current()); reply["ok"] = true; }
        else if (path == "/api/config/validate") {
            members(request, {"configuration"}); reply = impl_->validate(request, true);
            if (!reply.value("ok", false)) status = 422;
        } else if (path == "/api/config" && method == "PUT") {
            members(request, {"configuration", "expected"});
            const auto generation = candidate(request);
            std::lock_guard<std::mutex> mutation(impl_->writes);
            // Serialize reference admission with credential deletion. Checking
            // outside writes allowed DELETE to interleave before CAS Save.
            impl_->secrets.verifyReferences(generation->domain.snapshot());
            const auto saved = impl_->store.save(generation->domain.snapshot(), expected(request));
            reply = describe(saved); reply["ok"] = true; reply["applied"] = false;
        } else if (path == "/api/config/apply") {
            members(request, {"expected"});
            std::lock_guard<std::mutex> mutation(impl_->writes);
            const auto stored = impl_->store.current(); checkExpected(stored, expected(request));
            impl_->secrets.verifyReferences(stored);
            reply = impl_->invoke({{"method", "apply"}, {"configuration", Json::parse(stored->canonicalJson())}});
            if (!reply.value("ok", false)) status = 422;
            else {
                try {
                    impl_->store.recordLastKnownGood(stored);
                    impl_->health.setConfigRecovered(false);
                    impl_->health.setSubsystem("configuration", SubsystemStatus::Ready);
                    impl_->health.setSubsystem("runtime", SubsystemStatus::Ready);
                    impl_->health.setSubsystem("hardware_execution", SubsystemStatus::Ready);
                    if (reply.value("model_ready", false))
                        impl_->health.setSubsystem("llm", SubsystemStatus::Ready);
                    impl_->health.setPhase("ready");
                } catch (const std::exception& error) {
                    std::cerr << "Apply LKG record: " << error.what() << '\n';
                }
            }
        } else if (path == "/api/test-command") {
            if (request.contains("production")) members(request, {"text", "execute", "production"});
            else if (request.contains("execute")) members(request, {"text", "execute"});
            else members(request, {"text"});
            if (request.contains("execute") && !request["execute"].is_boolean()) throw std::invalid_argument("Execute must be boolean");
            if (request.contains("production") && !request["production"].is_boolean()) throw std::invalid_argument("Production must be boolean");
            if (!request["text"].is_string()) throw std::invalid_argument("Text must be string");
            request["method"] = "test";
            reply = impl_->invoke(request);
            if (!reply.value("ok", false)) status = 422;
        } else if (path == "/api/config/revisions") {
            reply = {{"ok", true}, {"revisions", Json::array()}};
            for (const auto& entry : impl_->store.revisions()) reply["revisions"].push_back({{"revision", entry.revision}, {"hash", entry.hash},{"timestamp_ms",entry.timestampMs}});
        } else if (path == "/api/config/restore") {
            members(request, {"revision", "expected"});
            if (!request["revision"].is_number_integer()) throw std::invalid_argument("Invalid revision");
            std::lock_guard<std::mutex> mutation(impl_->writes);
            const auto revision = request["revision"].get<std::int64_t>();
            // Restore writes a new ConfigStore revision. It must not
            // resurrect a broken credential reference without admission.
            impl_->secrets.verifyReferences(impl_->store.revision(revision));
            reply = describe(impl_->store.restore(revision, expected(request)));
            reply["ok"] = true; reply["applied"] = false;
        } else if (path == "/api/config/reconcile") {
            members(request, {});
            std::lock_guard<std::mutex> mutation(impl_->writes);
            const auto reconciled = impl_->store.reconcile();
            reply = {{"ok", true}, {"stored", reconciled ? describe(reconciled) : Json(nullptr)}, {"applied", false}};
        }
    } catch (const ApiError& error) { status = error.status; reply = failure(error.code, error.path, error.what()); }
    catch (const config::ConfigError& error) {
        status = error.code == "revision_conflict" ? 409 : error.code == "io_error" || error.code == "durability_indeterminate" || error.code == "recovery_required" ? 503 : 422;
        reply = failure(error.code, error.path, error.what());
    } catch (const std::runtime_error& error) {
        // These error codes are emitted by MqttCredentials as fixed literals:
        // never reflect a file path, account name or password in an API error.
        const std::string code(error.what());
        if (code.rfind("mqtt_broker_", 0) == 0) {
            status = code == "mqtt_broker_not_installed" ? 409 : 503;
            const auto safeCode = (code == "mqtt_broker_control_unavailable" ||
                                   code == "mqtt_broker_not_installed" ||
                                   code == "mqtt_broker_invalid_state" ||
                                   code == "mqtt_broker_control_failed")
                ? code : std::string("mqtt_broker_control_unavailable");
            reply = failure(safeCode, "/api/mqtt/local-broker",
                            "Локальный Mosquitto недоступен или не подтвердил изменение состояния.");
        } else if (code.rfind("mqtt_credentials_", 0) == 0) {
            const bool exists = code == "mqtt_credentials_exists";
            const bool unavailable = code == "mqtt_credentials_directory_unavailable" ||
                code == "mqtt_credentials_directory_permissions" ||
                code == "mqtt_credentials_write" ||
                code == "mqtt_credentials_read" ||
                code == "mqtt_credentials_delete" ||
                code == "mqtt_credentials_list" ||
                code == "mqtt_credentials_random_unavailable" ||
                code == "mqtt_credentials_atomic_create_unavailable" ||
                code == "mqtt_credentials_durability_uncertain";
            status = exists ? 409 : unavailable ? 503 : 422;
            reply = failure(code, "/mqtt/credentials",
                            exists ? "Этот ID уже существует." :
                            unavailable ? "Защищённое хранилище MQTT недоступно. Проверьте владельца и права каталога." :
                            "Учётные данные MQTT отсутствуют, повреждены или имеют недопустимый формат.");
        } else {
            std::cerr << "Admin request failed\n";
            status = 400;
            reply = failure("invalid_request", "", "Request must be one JSON object with the documented fields and types.");
        }
    } catch (const std::exception&) {
        // Never log or reflect user-controlled request body or secret content.
        status = 400;
        reply = failure("invalid_request", "", "Request must be one JSON object with the documented fields and types.");
    }
    return {status, "application/json; charset=utf-8", reply.dump(-1, ' ', false, Json::error_handler_t::replace)};
}
}
