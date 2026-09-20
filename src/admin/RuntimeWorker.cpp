#include "firefly/admin/RuntimeWorker.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/llm/StructuredOutput.h"
#include "Json.h"
#include <iostream>
#include <unistd.h>

namespace firefly::admin {
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
            members(request, {"method", "text"});
            const auto input = request.at("text").get<std::string>();
            const auto result = generations_.process(input);
            const auto& command = result.command;
            reply = {{"ok", command.error.empty()}, {"input", input},
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
            {"revision", catalog->configuration().revision}, {"hash", catalog->contentHash()}, {"context", active->prompt}};
    } else reply["active"] = nullptr;
    reply["model_ready"] = model_.ready();
    reply["worker_pid"] = ::getpid();
    reply["protocol_version"] = 1;
    return reply.dump(-1, ' ', false, Json::error_handler_t::replace);
}
}
