#include "firefly/admin/Api.h"
#include "firefly/admin/AdminTranscriber.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/config/ConfigValidator.h"
#include "Json.h"
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
    AdminTranscriber* transcriber = nullptr;
    HealthModel health;
    std::mutex writes, runtimeOperation, state;
    std::atomic<bool> busy{false};
    Json active = nullptr;
    bool ready = false;
    int workerPid = 0;
    std::string lastError;
    Impl(config::ConfigStore& store, RuntimeClient& runtime, AdminTranscriber* transcriber)
        : store(store), runtime(runtime), transcriber(transcriber) {}
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
        health.refreshOverall();
    }
    Json invoke(const Json& request) {
        std::unique_lock<std::mutex> operation(runtimeOperation, std::try_to_lock);
        if (!operation.owns_lock()) throw ApiError(409, "runtime_busy", "", "Runtime is processing another command or Apply. Retry when idle.");
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
        result["apply_ready"] = result["runtime_preflight"] == "verified";
        return result;
    }
};
Api::Api(config::ConfigStore& store, RuntimeClient& runtime, AdminTranscriber* transcriber)
    : impl_(std::make_unique<Impl>(store, runtime, transcriber)) {}
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
    std::cerr << "SHUTDOWN requested\n";
    impl_->health.setStopping(true);
}
void Api::initializeRuntime() {
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
            catalog = impl_->store.current();
            (void)std::make_shared<const RuntimeGeneration>(catalog);
            impl_->health.setSubsystem("configuration", SubsystemStatus::Ready);
        } catch (const std::exception& error) {
            configError = error.what();
            std::cerr << "STARTUP current config unusable: " << configError << '\n';
            try {
                auto lkg = impl_->store.lastKnownGood();
                if (lkg) {
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
HttpReply Api::handle(std::string_view method, std::string_view path, std::string_view body) {
    int status = 200;
    Json reply;
    try {
        const std::map<std::string, std::string> routes{{"/api/admin/plan", "POST"}, {"/api/admin/copilot", "POST"}, {"/api/admin/copilot/accept", "POST"}, {"/api/admin/transcribe", "POST"}, {"/api/admin/action-draft", "POST"}, {"/api/admin/templates", "GET"}, {"/api/admin/draft", "POST"}, {"/api/config/migrate", "POST"}, {"/api/admin/id", "POST"}, {"/api/admin/suggest", "POST"}, {"/api/config/diff", "POST"},
            {"/api/config/export", "GET"}, {"/api/config/import", "POST"}, {"/api/config/revision", "POST"},
            {"/api/config/restore-candidate", "POST"}, {"/api/dashboard", "GET"}, {"/api/devices/state", "GET"},
            {"/api/devices/observe", "POST"}, {"/api/executions", "GET"}, {"/api/status", "GET"}, {"/api/config", "GET PUT"},
            {"/api/config/active", "GET"}, {"/api/config/validate", "POST"}, {"/api/config/apply", "POST"},
            {"/api/config/revisions", "GET"}, {"/api/config/restore", "POST"}, {"/api/config/reconcile", "POST"}, {"/api/test-command", "POST"}};
        const auto route = routes.find(std::string(path));
        if (route == routes.end()) throw ApiError(404, "not_found", "", "Unknown API endpoint.");
        if ((" " + route->second + " ").find(" " + std::string(method) + " ") == std::string::npos)
            throw ApiError(405, "method_not_allowed", "", "HTTP method is not supported for this endpoint.");
        if (method == "GET" && !body.empty()) throw std::invalid_argument("Unexpected body");
        if (path == "/api/admin/transcribe") {
            if (!impl_->transcriber || !impl_->transcriber->ready())
                throw ApiError(503, "stt_unavailable", "", "Local speech recognition is not available on this Firefly.");
            reply = Json::parse(impl_->transcriber->transcribe(body));
            if (!reply.value("ok", false)) status = reply.contains("errors") &&
                reply["errors"].is_array() && !reply["errors"].empty() &&
                reply["errors"][0].value("code", "") == "stt_busy" ? 409 : 422;
            return {status, "application/json; charset=utf-8", reply.dump(-1, ' ', false, Json::error_handler_t::replace)};
        }
        Json request = method == "GET" ? Json::object() : parseMessage(body,true);
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
                    for (const auto* key : {"rooms","devices","actions","bindings"}) previous[key] = Json::array();
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
        } else if (path == "/api/devices/observe") {
            members(request, {"target_id", "properties"});
            Json payload{{"method","observe_state"},{"target_id",request.at("target_id")},{"properties",request.at("properties")}};
            if (request.contains("timestamp_ms")) payload["timestamp_ms"] = request.at("timestamp_ms");
            reply = impl_->invoke(payload);
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
            const auto saved = impl_->store.save(generation->domain.snapshot(), expected(request));
            reply = describe(saved); reply["ok"] = true; reply["applied"] = false;
        } else if (path == "/api/config/apply") {
            members(request, {"expected"});
            std::lock_guard<std::mutex> mutation(impl_->writes);
            const auto stored = impl_->store.current(); checkExpected(stored, expected(request));
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
            if (request.contains("execute")) members(request, {"text", "execute"});
            else members(request, {"text"});
            if (request.contains("execute") && !request["execute"].is_boolean()) throw std::invalid_argument("Execute must be boolean");
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
            reply = describe(impl_->store.restore(request["revision"].get<std::int64_t>(), expected(request)));
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
    } catch (const std::exception& error) {
        std::cerr << "Admin request: " << error.what() << '\n';
        status = 400; reply = failure("invalid_request", "", "Request must be one JSON object with the documented fields and types.");
    }
    return {status, "application/json; charset=utf-8", reply.dump(-1, ' ', false, Json::error_handler_t::replace)};
}
}
