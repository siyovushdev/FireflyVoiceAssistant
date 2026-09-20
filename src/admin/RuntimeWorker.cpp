#include "firefly/admin/RuntimeWorker.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/llm/StructuredOutput.h"
#include "Json.h"
#include <iostream>
#include <unistd.h>

namespace firefly::admin {
namespace {
Json stateJson(const execution::DeviceState& state) {
    if (const auto* light = std::get_if<execution::LightState>(&state)) return {{"brightness", light->brightness}};
    return {{"state", std::get<execution::DoorState>(state).position == execution::DoorPosition::Open ? "OPEN" : "CLOSED"}};
}
Json executionJson(const execution::ExecutionResult& result) {
    return {{"success", result.success}, {"actionId", result.actionId}, {"targetId", result.targetId},
        {"previous", result.previous ? stateJson(*result.previous) : Json(nullptr)},
        {"current", result.current ? stateJson(*result.current) : Json(nullptr)}, {"error", result.error}};
}
Json commandJson(const std::string& input, const GenerationResult& result) {
    const auto& command = result.command;
    Json reply = {{"ok", command.error.empty()}, {"input", input},
        {"route", command.path == CommandPath::FastPath ? "deterministic" :
            command.path == CommandPath::LlmFallback ? "rkllm" : "input_rejected"},
        {"result", Json::parse(llm::structuredResult(command.action))},
        {"candidate", Json::parse(llm::structuredResult(command.candidate))},
        {"safety_result", Json::parse(llm::structuredResult(command.safeCandidate))},
        {"decode", llm::decodeStatusName(command.decodeStatus)},
        {"safety", command.safetyRejected ? "REJECT" :
            std::holds_alternative<actions::GenericAction>(command.safeCandidate) ? "PASS" : "NO_ACTION"},
        {"domain", command.domainRejected ? "REJECT" :
            std::holds_alternative<actions::GenericAction>(command.action) ? "PASS" : "NO_ACTION"},
        {"latency_ms", command.latencyMs}, {"revision", result.generation->domain.snapshot()->configuration().revision},
        {"hash", result.generation->domain.snapshot()->contentHash()}};
    if (command.path == CommandPath::LlmFallback) reply["raw_rkllm"] = command.rawOutput;
    if (!command.error.empty()) {
        std::cerr << "Admin test runtime: " << command.error << '\n';
        reply["errors"] = failure("command_failed", "/text", "Command rejected or inference unavailable; inspect runtime diagnostics.")["errors"];
    }
    reply["generation_id"] = result.generation->id;
    reply["execution"] = result.execution ? executionJson(*result.execution) : Json(nullptr);
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
        } else if (method == "test") {
            if (request.contains("execute")) members(request, {"method", "text", "execute"});
            else members(request, {"method", "text"});
            if (request.contains("execute") && !request["execute"].is_boolean()) throw std::invalid_argument("Execute must be boolean");
            const auto input = request.at("text").get<std::string>();
            const auto admitted = generations_.beginRequest();
            const auto result = admitted->process(input, request.value("execute", false), "admin-test");
            reply = commandJson(input, result);
            remember(reply.dump());
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
    // Admission precedes STT. The request lease survives recognition, understanding
    // and execution, so Apply cannot splice two generations into one utterance.
    try {
        const auto request = generations_.beginRequest();
        voice::VoiceProcessor processor(recognizer, [&](std::string_view text) {
            const auto result = request->process(text, true, "voice");
            remember(commandJson(std::string(text), result).dump());
            return result.command;
        });
        auto result = processor.process(utterance);
        if (!result.error.empty()) remember(Json{{"input",result.recognition.text},{"error",result.error},{"result",{{"unsupported",true}}},
            {"generation_id",request->generation()->id},{"execution",nullptr}}.dump());
        return result;
    } catch (const std::exception&) {
        voice::VoiceResult result; result.error = "Voice runtime unavailable or stopping"; return result;
    }
}
std::string RuntimeWorker::observe() const {
    Json reply{{"ok",true},{"protocol_version",1},{"worker_pid",::getpid()},
        {"model_ready",generations_.modelReady()},{"runtime_busy",generations_.busy()},
        {"executor","mock"},{"devices",Json::array()},{"executions",Json::array()},{"active",nullptr}};
    if (const auto active = generations_.current()) {
        const auto& catalog = active->domain.snapshot();
        reply["active"] = {{"configuration",Json::parse(catalog->canonicalJson())},{"revision",catalog->configuration().revision},
            {"hash",catalog->contentHash()},{"context",active->prompt},{"generation_id",active->id}};
        for (const auto& device : active->executor->states()) reply["devices"].push_back(
            {{"targetId",device.targetId},{"name",device.name},{"state",stateJson(device.state)}});
    }
    for (const auto& event : generations_.history()) reply["executions"].push_back(
        {{"sequence",event.sequence},{"generation_id",event.generation},{"timestamp_ms",event.timestampMs},
         {"source",event.source},{"execution",executionJson(event.result)}});
    { std::lock_guard<std::mutex> lock(diagnostics_);
      reply["voice"] = voiceState_; reply["microphone"] = microphone_;
      reply["last_command"] = lastCommand_.empty() ? Json(nullptr) : Json::parse(lastCommand_); }
    return reply.dump(-1, ' ', false, Json::error_handler_t::replace);
}
}
