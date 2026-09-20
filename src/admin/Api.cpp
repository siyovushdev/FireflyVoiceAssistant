#include "firefly/admin/Api.h"
#include "firefly/config/ConfigLoader.h"
#include "Json.h"
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
        throw ApiError(422, "invalid_runtime_domain", "", "Domain is ambiguous, lacks a required light target, or exceeds the LLM context resource limit.");
    }
}
}
struct Api::Impl {
    config::ConfigStore& store;
    RuntimeClient& runtime;
    std::mutex writes, runtimeOperation, state;
    std::atomic<bool> busy{false};
    Json active = nullptr;
    bool ready = false;
    int workerPid = 0;
    std::string lastError;
    Impl(config::ConfigStore& store, RuntimeClient& runtime) : store(store), runtime(runtime) {}
    Json invoke(const Json& request) {
        std::unique_lock<std::mutex> operation(runtimeOperation, std::try_to_lock);
        if (!operation.owns_lock()) throw ApiError(409, "runtime_busy", "", "Runtime is processing another command or Apply. Retry when idle.");
        struct Busy { std::atomic<bool>& flag; explicit Busy(std::atomic<bool>& f) : flag(f) { flag = true; } ~Busy() { flag = false; } } guard(busy);
        Json reply;
        try {
            reply = parseMessage(runtime.call(request.dump()));
            if (reply.value("protocol_version", 0) != 1 || !reply.contains("active")) throw std::runtime_error("Runtime IPC version mismatch");
        } catch (const std::exception& error) {
            std::cerr << "Admin runtime IPC: " << error.what() << '\n';
            std::lock_guard<std::mutex> lock(state);
            ready = false; lastError = "Inference worker unavailable. Configuration editing remains available.";
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
Api::Api(config::ConfigStore& store, RuntimeClient& runtime) : impl_(std::make_unique<Impl>(store, runtime)) {}
Api::~Api() = default;
void Api::initializeRuntime() {
    try { (void)impl_->invoke({{"method", "apply"}, {"configuration", Json::parse(impl_->store.current()->canonicalJson())}}); }
    catch (const std::exception& error) { std::cerr << "Admin initial runtime: " << error.what() << '\n'; }
}
HttpReply Api::handle(std::string_view method, std::string_view path, std::string_view body) {
    int status = 200;
    Json reply;
    try {
        const std::map<std::string, std::string> routes{{"/api/status", "GET"}, {"/api/config", "GET PUT"},
            {"/api/config/active", "GET"}, {"/api/config/validate", "POST"}, {"/api/config/apply", "POST"},
            {"/api/config/revisions", "GET"}, {"/api/config/restore", "POST"}, {"/api/config/reconcile", "POST"}, {"/api/test-command", "POST"}};
        const auto route = routes.find(std::string(path));
        if (route == routes.end()) throw ApiError(404, "not_found", "", "Unknown API endpoint.");
        if ((" " + route->second + " ").find(" " + std::string(method) + " ") == std::string::npos)
            throw ApiError(405, "method_not_allowed", "", "HTTP method is not supported for this endpoint.");
        if (method == "GET" && !body.empty()) throw std::invalid_argument("Unexpected body");
        Json request = method == "GET" ? Json::object() : parseMessage(body);
        if (path == "/api/status") {
            reply = {{"ok", true}, {"runtime_busy", impl_->busy.load()}};
            try { const auto stored = impl_->store.current(); reply["stored"] = {{"revision", stored->configuration().revision}, {"hash", stored->contentHash()}}; }
            catch (const config::ConfigError& error) { reply["store_error"] = error.code; }
            std::lock_guard<std::mutex> lock(impl_->state);
            reply["runtime_ready"] = impl_->ready; reply["worker_pid"] = impl_->workerPid; reply["runtime_error"] = impl_->lastError;
            reply["active"] = impl_->active.is_null() ? Json(nullptr) : Json{{"revision", impl_->active["revision"]}, {"hash", impl_->active["hash"]}};
            reply["needs_apply"] = !reply.contains("stored") || reply["active"].is_null() || reply["stored"]["hash"] != reply["active"]["hash"];
        } else if (path == "/api/config/active") {
            std::lock_guard<std::mutex> lock(impl_->state);
            reply = {{"ok", true}, {"active", impl_->active}};
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
        } else if (path == "/api/test-command") {
            members(request, {"text"});
            if (!request["text"].is_string()) throw std::invalid_argument("Text must be string");
            reply = impl_->invoke({{"method", "test"}, {"text", request["text"]}});
            if (!reply.value("ok", false)) status = 422;
        } else if (path == "/api/config/revisions") {
            reply = {{"ok", true}, {"revisions", Json::array()}};
            for (const auto& entry : impl_->store.revisions()) reply["revisions"].push_back({{"revision", entry.revision}, {"hash", entry.hash}});
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
