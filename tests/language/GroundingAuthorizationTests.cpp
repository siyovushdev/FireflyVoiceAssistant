// Offline authorization/HTTP regression. Only inference and acoustic input are
// scripted; Api, RuntimeWorker, generations, SafetyGate and HTTP executor are real.
#include "../admin/Support.h"
#include "firefly/SafetyGate.h"
#include <iostream>

using namespace firefly;
using namespace admin_test;

namespace {
const std::string On = "action_lampa_vklyuchit", Off = "action_lampa_vyklyuchit";

struct Model final : admin::ContextModel {
    llm::CommandProtocol protocol = llm::CommandProtocol::StableCompact;
    std::string raw, prompt;
    int calls = 0;
    llm::CommandProtocol commandProtocol() const override { return protocol; }
    void useContext(const std::string& value) override { prompt = value; }
    bool ready() const override { return true; }
    std::string infer(std::string_view) override { ++calls; return raw; }
};

struct Case {
    std::string text;
    bool allowed;
    std::string variant = "single", target = "device_lampa", action = On;
};

Json configuration(Json base, const Case& test, int port) {
    auto room = base["rooms"][0];
    room["id"] = "location_bedroom"; room["name"] = "Спальня";
    room["speech"] = {{"mentions", {"спальня", "спальне"}}, {"locations", {"в спальне"}}};
    room["metadata"]["aliases"] = {"спальня"};
    base["rooms"].push_back(room);
    if (test.variant == "no-zone") base["devices"][0]["room_id"] = nullptr;
    if (test.variant == "socket") {
        base["devices"][0]["name"] = "Сетевая розетка 2";
        base["devices"][0]["spoken_aliases"] = {"сетевая розетка 2", "сетевую розетку 2"};
        base["devices"][0]["metadata"]["aliases"] = {"сетевая розетка 2"};
    }
    if (test.variant == "alias-location") base["devices"][0]["spoken_aliases"].push_back("лампу в подвале");
    if (test.variant == "action-location") base["actions"][0]["speech_aliases"].push_back("включи лампу в подвале");
    if (test.variant == "two-rooms" || test.variant == "same-room" || test.variant == "mixed-zone") {
        auto second = base["devices"][0]; second["id"] = "device_second";
        second["room_id"] = test.variant == "two-rooms" ? Json("location_bedroom") :
                            test.variant == "mixed-zone" ? Json(nullptr) : Json("location_kabinet");
        base["devices"].push_back(second);
    }
    base["connections"] = Json::array({{{"id", "connection_test"}, {"name", "Offline mock"},
        {"type", "http"}, {"enabled", true}, {"timeout_ms", 1000},
        {"http", {{"base_url", "http://127.0.0.1:" + std::to_string(port)}, {"base_path", ""}}}}});
    base["bindings"] = Json::array();
    for (const auto& d : base["devices"]) for (const auto& a : base["actions"])
        base["bindings"].push_back({{"target_id", d["id"]}, {"action_id", a["id"]},
            {"connection_id", "connection_test"}, {"executor", "http"},
            {"http", {{"method", "POST"}, {"path", "/relay/" + d["id"].get<std::string>()},
                {"body", {{"on", a["id"] == On}}}, {"expected_status", 200}}}});
    return base;
}

Json run(const Json& document, const Case& test, const std::string& path, llm::CommandProtocol protocol) {
    const auto snapshot = catalog(document);
    runtime::DomainView domain(snapshot);
    const actions::GenericAction proposed(test.action, test.target, {});
    Model model; model.protocol = protocol;
    // Deliberately adversarial: a real registered ID / [] even for the unknown
    // location. Default unsupported output would mask the original defect.
    model.raw = protocol == llm::CommandProtocol::Structured ? typed(proposed).dump() : "[]";
    Json response;
    bool accepted = false, executed = false;
    std::string route, source;
    if (path == "gate-injected" || path == "engine-no-index") {
        actions::CandidateResult action;
        if (path == "gate-injected") action = domain.validate(SafetyGate::apply(test.text, proposed, domain));
        else {
            DeterministicParser parser(domain);
            CommandEngine engine(model, parser, domain, [&](std::string_view raw) {
                return protocol == llm::CommandProtocol::Structured ? llm::StructuredDecoder(domain).decode(raw) :
                    llm::CompactActionDecoder(domain, llm::selectAction(domain, test.text)).decode(raw);
            });
            const auto result = engine.process(test.text); action = result.action;
            route = result.path == CommandPath::FastPath ? "LEGACY_FAST_PATH" : "LLM_FALLBACK";
        }
        response["result"] = typed(action);
        accepted = std::holds_alternative<actions::GenericAction>(action);
        if (accepted) {
            execution::BoundDeviceExecutor executor(snapshot);
            const auto result = executor.execute(std::get<actions::GenericAction>(action), execution::ExecutionIntent::Production);
            executed = result.success;
        }
    } else {
        admin::RuntimeWorker worker(model);
        admin::LocalRuntimeClient client(worker);
        Directory directory;
        std::unique_ptr<config::ConfigStore> store;
        std::unique_ptr<admin::Api> api;
        if (path == "api-text" || path == "api-device") {
            store = std::make_unique<config::ConfigStore>(directory.path); store->initialize(snapshot);
            api = std::make_unique<admin::Api>(*store, client); api->initializeRuntime();
        } else {
            const auto applied = Json::parse(worker.handle(Json{{"method", "apply"}, {"configuration", document}}.dump()));
            if (!applied.value("ok", false)) throw std::runtime_error(applied.dump());
        }
        if (path == "voice-seam") {
            struct Recognizer final : voice::SpeechRecognizer {
                std::string text;
                voice::RecognitionResult recognize(const std::vector<float>&) override { return {true, text, {}, 0}; }
            } recognizer;
            recognizer.text = "Амина, " + test.text;
            voice::Utterance utterance; utterance.audio.assign(16000, .2f); utterance.speech = {0, 16000}; utterance.inlineWake = true;
            worker.processVoice(recognizer, utterance);
            response = Json::parse(worker.observe()).at("last_voice").at("command");
        } else if (path == "api-device") {
            const auto reply = api->handle("POST", "/api/devices/command", Json{{"text", test.text},
                {"expected_hash", Json::parse(worker.observe()).at("active").at("hash")}, {"expected_action", {{"target_id", test.target},
                    {"action_id", test.action}, {"arguments", Json::object()}}}}.dump());
            response = Json::parse(reply.body);
        } else {
            Json request{{"text", test.text}, {"execute", true}, {"production", true}};
            if (path == "api-text") response = Json::parse(api->handle("POST", "/api/test-command", request.dump()).body);
            else { request["method"] = "test"; response = Json::parse(worker.handle(request.dump())); }
        }
        accepted = response.contains("result") && response["result"].contains("targetId");
        executed = response.contains("execution") && !response["execution"].is_null() && response["execution"].value("success", false);
        route = response.value("language_route", "");
        const auto history = Json::parse(worker.observe()).at("executions");
        if (executed) {
            if (history.size() != 1) throw std::runtime_error("Wrong execution history count");
            source = history[0].at("source");
            const auto expectedSource = path == "voice-seam" ? "voice" : path == "api-device" ? "admin-device" : "admin-production";
            if (source != expectedSource) throw std::runtime_error("Wrong entry point source");
        } else if (!history.empty()) throw std::runtime_error("Rejected request entered execution history");
    }
    // Operational controls intentionally disallow fallback, unlike text/voice.
    const bool allowed = test.allowed && !(path == "api-device" && test.text.find("будь добр") != std::string::npos);
    const bool identity = !accepted || (response["result"]["targetId"] == test.target && response["result"]["actionId"] == test.action);
    bool routeCorrect = true;
    if (allowed && path != "gate-injected" && path != "engine-no-index") {
        const bool fallback = test.text.find("будь добр") != std::string::npos;
        routeCorrect = route == (fallback ? "LLM_FALLBACK" : "INDEX_EXACT") && model.calls == (fallback ? 1 : 0);
    }
    if ((test.variant == "alias-location" || test.variant == "action-location") &&
        path != "gate-injected" && path != "engine-no-index")
        routeCorrect = route == "INDEX_EXACT" && model.calls == 0;
    return {{"input", test.text}, {"variant", test.variant}, {"path", path},
        {"protocol", protocol == llm::CommandProtocol::Structured ? "structured" : "stable_compact"},
        {"expected_post_count", allowed ? 1 : 0}, {"expected_on", test.action == On},
        {"expected_target", test.target}, {"expected_action", test.action},
        {"accepted", accepted}, {"executed", executed}, {"identity_correct", identity},
        {"pass", accepted == allowed && executed == allowed && identity && routeCorrect}, {"language_route", route},
        {"source", source}, {"model_calls", model.calls}, {"response", response}};
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("Repository root and loopback mock port required");
        const int port = std::stoi(argv[2]);
        const auto base = Json::parse(config::ConfigLoader::fromFile(std::string(argv[1]) + "/tests/language/m19-live-semantic.json")->canonicalJson());
        const std::vector<Case> cases{
            {"включи лампу в неизвестной комнате", false},
            {"включи лампу в кабинете", true}, {"выключи лампу в кабинете", true, "single", "device_lampa", Off},
            {"ВКЛЮЧИТЕ ЛАМПУ В КАБИНЕТЕ!", true}, {"включить лампа кабинет", true},
            {"пожалуйста, выключите лампу в кабинете", true, "single", "device_lampa", Off},
            {"будь добр включи лампу в кабинете", true},
            {"включи лампу в подвале", false}, {"включи лампу около гаража", false},
            {"включи лампу в кабинете в подвале", false}, {"включи лампу в кабинете неизвестной комнате", false},
            {"включи лампу в спальне", false}, {"включи лампу в кабинете и в спальне", false},
            {"включи лампу вместо спальни в кабинете", false}, {"включи лампу в", false},
            {"не включай лампу в кабинете", false}, {"включи и выключи лампу в кабинете", false},
            {"открой лампу в кабинете", false}, {"включи лампу 40 в кабинете", false},
            {"включи лампу", true, "no-zone"}, {"выключи лампу", true, "no-zone", "device_lampa", Off},
            {"будь добр включи лампу", true, "no-zone"},
            {"включи лампу в кабинете", false, "no-zone"}, {"включи лампу в подвале", false, "no-zone"},
            {"включи лампу", false, "two-rooms"}, {"включи лампу в неизвестной комнате", false, "two-rooms"},
            {"включи лампу в кабинете", true, "two-rooms"},
            {"выключи лампу в спальне", true, "two-rooms", "device_second", Off},
            {"включи лампу в кабинете", false, "same-room"}, {"включи лампу", false, "mixed-zone"},
            {"включи лампу в кабинете", true, "mixed-zone"},
            {"включи сетевую розетку 2 в кабинете", true, "socket"},
            {"выключи сетевую розетку 2 в кабинете", true, "socket", "device_lampa", Off},
            {"включи сетевую розетку 2 в подвале", false, "socket"},
            {"включи лампу в подвале", false, "alias-location"},
            {"включи лампу в подвале", false, "action-location"},
            {"включи лампу в лампу", false}
        };
        const auto emit = [&](const Case& test, const std::string& path, llm::CommandProtocol protocol) {
            const auto report = run(configuration(base, test, port), test, path, protocol);
            std::cout << report.dump() << std::endl;
            std::string acknowledgment;
            if (!std::getline(std::cin, acknowledgment) || acknowledgment != "checked")
                throw std::runtime_error("HTTP observer did not acknowledge case");
        };
        for (const auto protocol : {llm::CommandProtocol::StableCompact, llm::CommandProtocol::Structured})
            for (const auto& path : {"api-text", "api-device", "worker-text", "voice-seam", "engine-no-index", "gate-injected"})
                for (const auto& test : cases) emit(test, path, protocol);
        // A model can return a registered device in a different room even though
        // the original request uniquely names the first. Registered IDs are data,
        // never authority; all text/voice/operational lanes must reject this.
        const Case forged{"будь добр включи лампу в кабинете", false, "two-rooms", "device_second", On};
        for (const auto& path : {"api-text", "api-device", "worker-text", "voice-seam", "engine-no-index", "gate-injected"})
            emit(forged, path, llm::CommandProtocol::Structured);
        return 0;
    } catch (const config::ConfigError& error) {
        std::cerr << error.code << " " << error.path << ": " << error.what() << '\n'; return 1;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
