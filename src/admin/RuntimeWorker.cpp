#include "firefly/admin/RuntimeWorker.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/llm/StructuredOutput.h"
#include "Json.h"
#include "AdminBuild.h"
#include "firefly/admin/AdminDraft.h"
#include "firefly/admin/AdminCopilot.h"
#include "firefly/actions/ScalarJson.h"
#include <iostream>
#include <algorithm>
#include <unistd.h>

namespace firefly::admin {
namespace {
Json stateJson(const execution::DeviceState& state) {
    if (const auto* fields = std::get_if<execution::TypedState>(&state)) {
        Json result = Json::object();
        for (const auto& [name, value] : *fields) result[name] = actions::scalarJson(value);
        return result;
    }
    if (const auto* light = std::get_if<execution::LightState>(&state)) return {{"brightness", light->brightness}};
    if (std::holds_alternative<execution::InventoryState>(state)) return {{"status", "unconfigured"}};
    return {{"state", std::get<execution::DoorState>(state).position == execution::DoorPosition::Open ? "OPEN" : "CLOSED"}};
}
Json propertyKnowledgeJson(const execution::PropertyKnowledge& property, std::int64_t nowMs) {
    Json value = nullptr;
    if (property.value) value = actions::scalarJson(*property.value);
    return {{"value", value},
        {"knowledge", execution::DeviceStateStore::knowledgeName(property.knowledge)},
        {"stale", execution::isStale(property, nowMs)},
        {"updated_at_ms", property.updatedAtMs},
        {"observed_at_ms", property.observedAtMs > 0 ? Json(property.observedAtMs) : Json(nullptr)}};
}
Json deviceKnowledgeJson(const execution::DeviceKnowledge& device, std::int64_t nowMs) {
    Json properties = Json::object();
    Json legacy = Json::object();
    for (const auto& [key, property] : device.properties) {
        properties[key] = propertyKnowledgeJson(property, nowMs);
        if (property.value) {
            if (key == "position") legacy["state"] = actions::scalarJson(*property.value);
            else legacy[key] = actions::scalarJson(*property.value);
        }
    }
    if (device.properties.empty()) legacy = {{"status", "unconfigured"}};
    return {{"targetId", device.targetId}, {"name", device.name},
        {"properties", properties},
        {"state", legacy},
        {"connectivity", execution::DeviceStateStore::connectivityName(device.connectivity)},
        {"last_success_at_ms", device.lastSuccessAtMs > 0 ? Json(device.lastSuccessAtMs) : Json(nullptr)},
        {"last_failure_at_ms", device.lastFailureAtMs > 0 ? Json(device.lastFailureAtMs) : Json(nullptr)}};
}
Json executionJson(const execution::ExecutionResult& result) {
    return {{"success", result.success},
        {"status", result.status == execution::ExecutionStatus::Accepted ? "accepted" : "failed"},
        {"actionId", result.actionId}, {"targetId", result.targetId},
        {"previous", result.previous ? stateJson(*result.previous) : Json(nullptr)},
        {"current", result.current ? stateJson(*result.current) : Json(nullptr)},
        {"error", result.error}, {"timestamp_ms", result.timestampMs}};
}
Json commandJson(const std::string& input, const GenerationResult& result) {
    const auto& command = result.command;
    Json reply = {{"ok", command.error.empty() || command.domainRejected || command.path == CommandPath::GroundingRejected}, {"input", input},
        {"route", command.path == CommandPath::FastPath ? "deterministic" :
            command.path == CommandPath::LlmFallback ? "rkllm" : command.path == CommandPath::GroundingRejected ? "grounding_rejected" : "input_rejected"},
        {"result", Json::parse(llm::structuredResult(command.action))},
        {"candidate", Json::parse(llm::structuredResult(command.candidate))},
        {"safety_result", Json::parse(llm::structuredResult(command.safeCandidate))},
        {"decode", llm::decodeStatusName(command.decodeStatus)},
        {"safety", command.safetyRejected ? "REJECT" :
            std::holds_alternative<actions::GenericAction>(command.safeCandidate) ? "PASS" : "NO_ACTION"},
        {"domain", command.domainRejected || command.decodeStatus == llm::DecodeStatus::OutsideDomain ? "REJECT" :
            std::holds_alternative<actions::GenericAction>(command.action) ? "PASS" : "NO_ACTION"},
        {"llm", {{"context_ms",command.llmTimings.contextMs},{"prefill_ms",command.llmTimings.prefillMs},
            {"generation_ms",command.llmTimings.generationMs},{"output_tokens",command.llmTimings.outputTokens}}},
        {"latency_ms", command.latencyMs}, {"revision", result.generation->domain.snapshot()->configuration().revision},
        {"hash", result.generation->domain.snapshot()->contentHash()}};
    if (command.path == CommandPath::LlmFallback) {
        reply["raw_rkllm"] = command.rawOutput;
        reply["llm_context"] = result.generation->requestPrompt(input);
    }
    if (!command.error.empty() && !command.domainRejected && command.path != CommandPath::GroundingRejected) {
        std::cerr << "Admin test runtime: " << command.error << '\n';
        reply["errors"] = failure(command.path == CommandPath::InputRejected ? "invalid_input" : "command_failed", "/text", command.error)["errors"];
    }
    reply["matching_evidence"] = Json::array();
    for (const auto& m : command.grounding.mentions) reply["matching_evidence"].push_back({
        {"entity_id",m.entityId},{"category",m.category},{"alias",m.alias},{"span",m.span},{"method",m.method},
        {"begin",m.begin},{"end",m.end},{"distance",m.distance},{"generation_id",result.generation->id},{"catalog_hash",command.grounding.catalogHash}});
    reply["ambiguous"] = command.grounding.ambiguous;
    reply["reason"] = command.reason;
    reply["rejection_stage"] = command.path == CommandPath::GroundingRejected ? "grounding" : command.safetyRejected ? "safety" : command.domainRejected || command.decodeStatus == llm::DecodeStatus::OutsideDomain ? "domain" :
        command.invalidModelOutput ? "decoder" : command.decodeStatus == llm::DecodeStatus::Unsupported ? "model_unsupported" :
        !command.error.empty() ? "runtime" : "";
    reply["generation_id"] = result.generation->id;
    reply["execution"] = result.execution ? executionJson(*result.execution) : Json(nullptr);
    if (result.execution && !result.execution->success) reply["rejection_stage"] = "executor";
    reply["predicted_execution"] = nullptr;
    if (!result.execution) if (const auto* action = std::get_if<actions::GenericAction>(&command.action)) {
        reply["predicted_execution"] = executionJson(result.generation->executor->preview(*action));
        reply["predicted_execution"]["executed"] = false;
    }
    return reply;
}
}

std::string RuntimeWorker::handle(std::string_view bytes) {
    Json reply;
    try {
        const auto request = parseMessage(bytes);
        const auto method = request.at("method").get<std::string>();
        if (method == "apply" || method == "preflight") {
            members(request, {"method", "configuration"});
            auto catalog = config::ConfigLoader::fromJson(request.at("configuration").dump());
            if (method == "apply") generations_.apply(std::move(catalog));
            else generations_.preflight(std::move(catalog));
            reply = {{"ok", true}};
        } else if (method == "state_diff") {
            members(request,{"method","configuration"});
            const auto next = config::ConfigLoader::fromJson(request.at("configuration").dump());
            execution::MockDeviceExecutor preview(next);
            Json changes = Json::array();std::vector<execution::DeviceRecord> before;
            if (const auto active = generations_.current()) before = active->executor->states();
            try {
                preview.reconcile(before);
                const auto after = preview.states();
                for (const auto& old : before) {
                    const auto found = std::find_if(after.begin(),after.end(),[&](const auto& d){return d.targetId==old.targetId;});
                    if (found == after.end()) changes.push_back({{"entity","state"},{"name",old.name},{"change","changed"},{"before",{{"state",stateJson(old.state)}}},{"after",{{"state","Устройство удаляется"}}}});
                    else if (!(found->state == old.state)) changes.push_back({{"entity","state"},{"name",found->name},{"change","changed"},{"before",{{"state",stateJson(old.state)}}},{"after",{{"state",stateJson(found->state)}}}});
                }
                for (const auto& added : after) if (std::none_of(before.begin(),before.end(),[&](const auto& d){return d.targetId==added.targetId;}))
                    changes.push_back({{"entity","state"},{"name",added.name},{"change","changed"},{"before",{{"state",nullptr}}},{"after",{{"state",stateJson(added.state)}}}});
                reply = {{"ok",true},{"compatible",true},{"changes",changes},{"policy","preserve_or_reject; initialize_new_or_inventory; remove_deleted"}};
            } catch (const config::ConfigError& error) {
                changes.push_back({{"entity","state"},{"name","Apply будет отклонён"},{"change","changed"},{"before",{{"state","Рабочее состояние сохраняется"}}},{"after",{{"state",error.what()}}}});
                reply = {{"ok",true},{"compatible",false},{"changes",changes},{"error",error.what()}};
            }
        } else if (method == "plan" || method == "action_draft" || method == "copilot") {
            if (method == "plan") members(request,{"method","text","configuration"});
            else if (method == "copilot") members(request,{"method","text","configuration"});
            else members(request,{"method","name","target","configuration"});
            const auto candidate = config::ConfigLoader::fromJson(request.at("configuration").dump());
            if (method == "copilot" && request.contains("draft_hash")) {
                if (!request.at("draft_hash").is_string() ||
                    request.at("draft_hash").get<std::string>() != draftFingerprint(candidate))
                    throw config::ConfigError("stale_proposal", "/draft_hash",
                        "Copilot draft_hash does not match the submitted configuration. Regenerate.");
            }
            const auto prompt = method == "plan" ? planPrompt() : method == "copilot" ? copilotPrompt(candidate)
                : actionDraftPrompt(candidate,request.at("target").get<std::string>());
            const auto input = request.at(method == "action_draft" ? "name" : "text").get<std::string>();
            const int budget = 384;
            reply = Json::parse(generations_.structuredSuggestion(prompt,input,budget,[&](std::string_view raw) {
                return method == "plan" ? decodePlan(raw,candidate) : method == "copilot" ? decodeCopilotProposal(raw,candidate) :
                    decodeActionDraft(raw,candidate,request.at("target").get<std::string>(),input);
            }));
            if ((method == "plan" || method == "copilot") && reply.contains("suggestion")) {
                reply["draft"] = reply.at("suggestion"); reply.erase("suggestion");
            }
            if (method == "copilot" && reply.value("ok", false) && reply.contains("draft") &&
                reply.at("draft").value("rejected", false)) {
                reply["ok"] = false;
                const auto& draft = reply.at("draft");
                const auto code = draft.value("code", draft.at("status").get<std::string>());
                reply["errors"] = failure(code, "", draft.at("summary").get<std::string>())["errors"];
            }
        } else if (method == "draft") {
            members(request, {"method", "entity", "name"});
            reply = Json::parse(generations_.draft(request.at("entity").get<std::string>(), request.at("name").get<std::string>()));
        } else if (method == "suggest") {
            members(request, {"method", "entity", "name"});
            const auto start = std::chrono::steady_clock::now();
            const auto entity = request.at("entity").get<std::string>();
            reply = Json::parse(generations_.structuredSuggestion(suggestionPrompt(entity),
                request.at("name").get<std::string>(),128,
                [&](std::string_view raw){return decodeSuggestion(raw,entity);}));
            reply["raw_rkllm"] = reply.at("trace").back().at("raw");
            reply["latency_ms"] = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            reply["saved"] = false; reply["applied"] = false;
        } else if (method == "test") {
            if (request.contains("production")) members(request, {"method", "text", "execute", "production"});
            else if (request.contains("execute")) members(request, {"method", "text", "execute"});
            else members(request, {"method", "text"});
            if (request.contains("execute") && !request["execute"].is_boolean()) throw std::invalid_argument("Execute must be boolean");
            if (request.contains("production") && !request["production"].is_boolean()) throw std::invalid_argument("Production must be boolean");
            const auto input = request.at("text").get<std::string>();
            const auto admitted = generations_.beginRequest();
            // Default Admin execute stays Playground. Explicit production=true exercises
            // BindingResolver → HardwareExecutor → DeviceStateStore (V1 demo / voice parity).
            execution::ExecutionIntent intent = execution::ExecutionIntent::None;
            if (request.value("execute", false))
                intent = request.value("production", false)
                    ? execution::ExecutionIntent::Production
                    : execution::ExecutionIntent::Playground;
            const auto result = admitted->process(input, intent, request.value("production", false) ? "admin-production" : "admin-test");
            reply = commandJson(input, result);
            remember(reply.dump());
        } else if (method == "observe_state") {
            // Simulated telemetry/readback for M15 offline verification — not a transport.
            members(request, {"method", "target_id", "properties"});
            if (!request["target_id"].is_string() || !request["properties"].is_object())
                throw std::invalid_argument("observe_state requires target_id string and properties object");
            execution::StateObservation observation;
            observation.targetId = request.at("target_id").get<std::string>();
            for (auto it = request.at("properties").begin(); it != request.at("properties").end(); ++it)
                observation.properties.emplace(it.key(), actions::decodeScalar(it.value()));
            if (request.contains("timestamp_ms")) {
                if (!request["timestamp_ms"].is_number_integer())
                    throw std::invalid_argument("timestamp_ms must be integer");
                observation.timestampMs = request["timestamp_ms"].get<std::int64_t>();
            }
            generations_.observe(observation);
            reply = {{"ok", true}, {"target_id", observation.targetId}};
        } else if (method == "observe") {
            members(request, {"method"}); reply = Json::parse(observe());
        } else throw std::invalid_argument("Unknown runtime method");
    } catch (const config::ConfigError& error) {
        reply = failure(error.code, error.path, error.what());
    } catch (const std::exception& error) {
        std::cerr << "Admin runtime: " << error.what() << '\n';
        reply = failure("runtime_rejected", "", "Runtime context or request rejected. The previous generation was not replaced.");
    }
    if (const auto active = generations_.current()) {
        const auto& catalog = active->domain.snapshot();
        reply["active"] = {{"configuration", Json::parse(catalog->canonicalJson())},
            {"revision", catalog->configuration().revision}, {"hash", catalog->contentHash()}, {"context", active->prompt}, {"generation_id", active->id}};
    } else reply["active"] = nullptr;
    reply["model_ready"] = generations_.modelReady();
    reply["worker_pid"] = ::getpid();
    reply["protocol_version"] = 1;
    return reply.dump(-1, ' ', false, Json::error_handler_t::replace);
}

void RuntimeWorker::remember(std::string diagnostic) {
    auto bounded = Json::parse(diagnostic);
    if (bounded.contains("input") && bounded["input"].is_string() && bounded["input"].get_ref<const std::string&>().size() > 192)
        bounded["input"] = "[oversized rejected input omitted]";
    std::lock_guard<std::mutex> lock(diagnostics_); lastCommand_ = bounded.dump();
}
void RuntimeWorker::voiceStatus(std::string state, std::string microphone) {
    std::lock_guard<std::mutex> lock(diagnostics_);
    voiceState_ = std::move(state); microphone_ = std::move(microphone);
}
voice::VoiceResult RuntimeWorker::processVoice(voice::SpeechRecognizer& recognizer, const voice::Utterance& utterance) {
    const auto started = std::chrono::steady_clock::now();
    const auto elapsedMs = [&] { return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now() - started).count(); };
    // Admission precedes STT. The request lease survives recognition, understanding
    // and execution, so Apply cannot splice two generations into one utterance.
    try {
        const auto request = generations_.beginRequest();
        std::string voiceCommand;
        voice::VoiceProcessor processor(recognizer, [&](std::string_view text) {
            const auto result = request->process(text, execution::ExecutionIntent::Production, "voice");
            voiceCommand = commandJson(std::string(text), result).dump();
            remember(voiceCommand);
            return result.command;
        });
        auto result = processor.process(utterance);
        result.processingMs = elapsedMs(); // Includes serialized admission wait, STT and execution.
        { std::lock_guard<std::mutex> lock(diagnostics_);
          lastVoice_ = Json{{"audio_detected",true},{"duration_seconds",result.durationSeconds},
              {"processing_ms",result.processingMs},{"speech_seconds",result.speechSeconds},{"stt_ms",result.recognition.latencyMs},{"transcript",result.recognition.text},{"inline",result.inlineWake},
              {"wake_prefix_removed",result.wakePrefixRemoved},{"command_text",result.commandText},
              {"error",result.error},{"command",voiceCommand.empty()?Json(nullptr):Json::parse(voiceCommand)}}.dump(); }
        if (!result.error.empty()) remember(Json{{"input",result.recognition.text},{"error",result.error},{"result",{{"unsupported",true}}},
            {"generation_id",request->generation()->id},{"execution",nullptr}}.dump());
        return result;
    } catch (const std::exception&) {
        voice::VoiceResult result; result.error = "Voice runtime unavailable or stopping"; result.processingMs = elapsedMs(); return result;
    }
}
std::string RuntimeWorker::observe() const {
    Json reply{{"ok",true},{"protocol_version",1},{"worker_pid",::getpid()},
        {"model_ready",generations_.modelReady()},{"runtime_busy",generations_.busy()},
        {"uptime_seconds",std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-started_).count()},
        {"context_tokens",generations_.contextTokens()},{"context_limit",1024},
        {"executor","bound"},{"devices",Json::array()},{"playground_devices",Json::array()},
        {"executions",Json::array()},{"active",nullptr}};
    reply["build"] = {{"product","Firefly Milestone 17 · Runtime service reliability"},{"head",BuildHead},{"source_sha256",BuildSource},{"built_utc",BuildTime}};
    if (const auto active = generations_.current()) {
        const auto& catalog = active->domain.snapshot();
        reply["active"] = {{"configuration",Json::parse(catalog->canonicalJson())},{"revision",catalog->configuration().revision},
            {"hash",catalog->contentHash()},{"context",active->prompt},{"context_strategy",active->targetContexts.empty()?"full_catalog":"explicit_target_action_scopes"},{"context_scope_count",active->targetContexts.size()},{"generation_id",active->id}};
        const auto now = active->executor->production().nowMs();
        for (const auto& device : active->executor->production().devices())
            reply["devices"].push_back(deviceKnowledgeJson(device, now));
        for (const auto& device : active->executor->states())
            reply["playground_devices"].push_back(
                {{"targetId",device.targetId},{"name",device.name},{"state",stateJson(device.state)}});
    }
    for (const auto& event : generations_.history()) reply["executions"].push_back(
        {{"sequence",event.sequence},{"generation_id",event.generation},{"timestamp_ms",event.timestampMs},
         {"source",event.source},{"execution",executionJson(event.result)}});
    { std::lock_guard<std::mutex> lock(diagnostics_);
      reply["last_voice"] = lastVoice_.empty() ? Json(nullptr) : Json::parse(lastVoice_);
      reply["voice"] = voiceState_; reply["microphone"] = microphone_;
      reply["last_command"] = lastCommand_.empty() ? Json(nullptr) : Json::parse(lastCommand_); }
    return reply.dump(-1, ' ', false, Json::error_handler_t::replace);
}
}
