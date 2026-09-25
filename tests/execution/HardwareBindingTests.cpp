#include "../admin/Support.h"
#include "firefly/execution/ActionExecutor.h"
#include "firefly/execution/HardwareExecutor.h"
#include "firefly/execution/HttpHardwareExecutor.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/config/ConfigValidator.h"
#include <atomic>
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace firefly;
using namespace admin_test;

namespace {
std::atomic<int> checks{0};
void check(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}

struct FakeHttpServer {
    int port = 0;
    std::atomic<bool> running{true};
    std::string lastMethod, lastPath, lastBody, lastContentType;
    int status = 200;
    int delayMs = 0;
    std::thread thread;
    int listenFd = -1;

    explicit FakeHttpServer(int responseStatus = 200) : status(responseStatus) {
        listenFd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listenFd < 0) throw std::runtime_error("socket");
        int on = 1;
        ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listenFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
            throw std::runtime_error("bind");
        socklen_t length = sizeof(address);
        if (::getsockname(listenFd, reinterpret_cast<sockaddr*>(&address), &length) != 0)
            throw std::runtime_error("getsockname");
        port = ntohs(address.sin_port);
        if (::listen(listenFd, 8) != 0) throw std::runtime_error("listen");
        thread = std::thread([this] { serve(); });
    }
    ~FakeHttpServer() {
        running = false;
        if (listenFd >= 0) ::shutdown(listenFd, SHUT_RDWR);
        if (thread.joinable()) thread.join();
        if (listenFd >= 0) ::close(listenFd);
    }
    void serve() {
        while (running) {
            pollfd descriptor{listenFd, POLLIN, 0};
            if (::poll(&descriptor, 1, 100) <= 0) continue;
            const int client = ::accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0) continue;
            if (delayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            std::string bytes;
            while (bytes.find("\r\n\r\n") == std::string::npos && bytes.size() < 65536) {
                char buffer[4096];
                const auto count = ::recv(client, buffer, sizeof(buffer), 0);
                if (count <= 0) break;
                bytes.append(buffer, static_cast<std::size_t>(count));
            }
            const auto headerEnd = bytes.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                std::istringstream first(bytes.substr(0, bytes.find("\r\n")));
                first >> lastMethod >> lastPath;
                lastBody = bytes.substr(headerEnd + 4);
                const auto type = bytes.find("Content-Type:");
                if (type != std::string::npos) {
                    auto line = bytes.substr(type, bytes.find("\r\n", type) - type);
                    const auto colon = line.find(':');
                    lastContentType = line.substr(colon + 1);
                    while (!lastContentType.empty() && lastContentType.front() == ' ') lastContentType.erase(lastContentType.begin());
                }
                std::size_t length = 0;
                const auto cl = bytes.find("Content-Length:");
                if (cl != std::string::npos) {
                    try { length = static_cast<std::size_t>(std::stoul(bytes.substr(cl + 15))); } catch (...) {}
                }
                while (lastBody.size() < length && lastBody.size() < 65536) {
                    char buffer[4096];
                    const auto count = ::recv(client, buffer, sizeof(buffer), 0);
                    if (count <= 0) break;
                    lastBody.append(buffer, static_cast<std::size_t>(count));
                }
                if (lastBody.size() > length) lastBody.resize(length);
            }
            const std::string reply = "HTTP/1.1 " + std::to_string(status) + " OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
            ::send(client, reply.data(), reply.size(), MSG_NOSIGNAL);
            ::close(client);
        }
    }
};

Json baseV2(const std::string& root) {
    auto value = base(root);
    return Json::parse(config::ConfigValidator::validate(config::migrateV2(config::ConfigLoader::fromJson(value.dump())->configuration()))->canonicalJson());
}

void addPowerAction(Json& value) {
    value["actions"].push_back({
        {"id", "action_lamp_on"}, {"name", "Включить лампу"}, {"operation", "mock.assign.v1"}, {"enabled", true},
        {"description", "Включает виртуальную лампу."}, {"speech_aliases", {"включи лампу"}},
        {"parameters", Json::array()},
        {"assignments", {{{"field", "power"}, {"value", true}}}}
    });
    value["devices"].push_back({
        {"id", "device_office_lamp"}, {"name", "Лампа кабинета"}, {"kind", "generic"},
        {"room_id", "room_kitchen"}, {"enabled", true}, {"action_ids", {"action_lamp_on"}},
        {"spoken_aliases", {"лампа"}},
        {"state", {{{"id", "power"}, {"name", "Питание"}, {"type", "boolean"}, {"required", true},
            {"description", ""}, {"unit", ""}, {"initial", false}}}},
        {"metadata", {{"type", "light"}, {"description", ""}, {"aliases", Json::array()}}}
    });
}

Json binding(const std::string& url, int timeoutMs = 1000, const char* argument = nullptr) {
    Json http{{"method", "POST"}, {"url", url}, {"timeout_ms", timeoutMs}, {"content_type", "application/json"},
        {"body", {{"power", true}}}};
    if (argument) http["argument"] = argument;
    return {{"target_id", "device_office_lamp"}, {"action_id", "action_lamp_on"}, {"executor", "http"}, {"http", http}};
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        const auto root = argv[1];

        // --- Config validation ---
        {
            auto value = baseV2(root);
            addPowerAction(value);
            FakeHttpServer server;
            value["bindings"] = Json::array({binding("http://127.0.0.1:" + std::to_string(server.port) + "/relay")});
            const auto catalog = config::ConfigLoader::fromJson(value.dump());
            check(catalog->bindingIndex().count({"device_office_lamp", "action_lamp_on"}), "valid HTTP binding");
        }
        {
            auto value = baseV2(root); addPowerAction(value);
            value["bindings"] = Json::array({binding("http://127.0.0.1:9/relay")});
            value["bindings"][0]["target_id"] = "device_missing";
            try { (void)config::ConfigLoader::fromJson(value.dump()); check(false, "unknown target"); }
            catch (const config::ConfigError& error) { check(error.code == "dangling_reference", "unknown target code"); }
        }
        {
            auto value = baseV2(root); addPowerAction(value);
            value["bindings"] = Json::array({binding("http://127.0.0.1:9/relay")});
            value["bindings"][0]["action_id"] = "action_missing";
            try { (void)config::ConfigLoader::fromJson(value.dump()); check(false, "unknown action"); }
            catch (const config::ConfigError& error) { check(error.code == "dangling_reference", "unknown action code"); }
        }
        {
            auto value = baseV2(root); addPowerAction(value);
            value["bindings"] = Json::array({binding("http://127.0.0.1:9/a"), binding("http://127.0.0.1:9/b")});
            try { (void)config::ConfigLoader::fromJson(value.dump()); check(false, "duplicate binding"); }
            catch (const config::ConfigError& error) { check(error.code == "duplicate_binding", "duplicate binding code"); }
        }
        {
            auto value = baseV2(root); addPowerAction(value);
            auto item = binding("http://127.0.0.1:9/relay");
            item["executor"] = "mqtt";
            value["bindings"] = Json::array({item});
            try { (void)config::ConfigLoader::fromJson(value.dump()); check(false, "unknown executor"); }
            catch (const config::ConfigError& error) { check(error.code == "schema_error" || error.code == "unsupported_executor", "unknown executor code"); }
        }
        {
            auto value = baseV2(root); addPowerAction(value);
            value["bindings"] = Json::array({binding("ftp://127.0.0.1/relay")});
            try { (void)config::ConfigLoader::fromJson(value.dump()); check(false, "malformed url"); }
            catch (const config::ConfigError& error) { check(error.code == "malformed_binding", "malformed url code"); }
        }
        {
            auto value = baseV2(root); addPowerAction(value);
            auto item = binding("http://127.0.0.1:9/relay");
            item["http"]["argument"] = "brightness";
            value["bindings"] = Json::array({item});
            try { (void)config::ConfigLoader::fromJson(value.dump()); check(false, "bad argument"); }
            catch (const config::ConfigError& error) { check(error.code == "dangling_reference", "bad argument code"); }
        }

        // --- Resolver ---
        {
            auto value = baseV2(root); addPowerAction(value);
            value["bindings"] = Json::array({binding("http://127.0.0.1:9/relay")});
            const auto catalog = config::ConfigLoader::fromJson(value.dump());
            execution::BindingResolver resolver(catalog);
            const auto found = resolver.resolve("device_office_lamp", "action_lamp_on");
            check(found.status == execution::BindingResolver::Status::Found && found.binding, "resolver found");
            const auto missing = resolver.resolve("device_office_lamp", "action_set_brightness");
            check(missing.status == execution::BindingResolver::Status::Missing, "resolver missing");
        }

        // --- HTTP executor + BoundDeviceExecutor pipeline ---
        {
            FakeHttpServer server;
            auto value = baseV2(root); addPowerAction(value);
            value["bindings"] = Json::array({binding("http://127.0.0.1:" + std::to_string(server.port) + "/gpio/led")});
            const auto catalog = config::ConfigLoader::fromJson(value.dump());
            execution::BoundDeviceExecutor executor(catalog);
            actions::GenericAction action{"action_lamp_on", "device_office_lamp", {}};
            const auto preview = executor.preview(action);
            check(preview.success, "preview semantic success");
            check(std::get<execution::TypedState>(executor.states().back().state).at("power") == actions::Scalar(false), "preview no mutation");
            const auto result = executor.execute(action);
            check(result.success && result.status == execution::ExecutionStatus::Accepted, "hardware execute success");
            check(server.lastMethod == "POST" && server.lastPath == "/gpio/led", "correct path");
            check(server.lastBody.find("\"power\":true") != std::string::npos, "correct body");
            // Production Accepted → Predicted in DeviceStateStore; playground mock unchanged.
            check(std::get<execution::TypedState>(executor.states().back().state).at("power") == actions::Scalar(false),
                "playground unchanged after production accept");
            const auto knowledge = executor.production().device("device_office_lamp");
            check(knowledge && knowledge->properties.at("power").knowledge == execution::StateKnowledge::Predicted &&
                knowledge->properties.at("power").value == actions::Scalar(true), "production Predicted ON");
            check(knowledge->connectivity == execution::Connectivity::Reachable, "production Reachable");
        }
        {
            FakeHttpServer server(500);
            auto value = baseV2(root); addPowerAction(value);
            value["bindings"] = Json::array({binding("http://127.0.0.1:" + std::to_string(server.port) + "/fail")});
            execution::BoundDeviceExecutor executor(config::ConfigLoader::fromJson(value.dump()));
            const auto result = executor.execute({"action_lamp_on", "device_office_lamp", {}});
            check(!result.success && result.error.find("http_error") != std::string::npos, "http error");
            check(std::get<execution::TypedState>(executor.states().back().state).at("power") == actions::Scalar(false), "http error no playground state");
            const auto knowledge = executor.production().device("device_office_lamp");
            check(knowledge && knowledge->properties.at("power").knowledge == execution::StateKnowledge::Unknown, "http error no prediction");
            check(knowledge->connectivity == execution::Connectivity::Reachable, "HTTP 500 → Reachable");
        }
        {
            FakeHttpServer server;
            server.delayMs = 400;
            auto value = baseV2(root); addPowerAction(value);
            value["bindings"] = Json::array({binding("http://127.0.0.1:" + std::to_string(server.port) + "/slow", 100)});
            execution::BoundDeviceExecutor executor(config::ConfigLoader::fromJson(value.dump()));
            const auto result = executor.execute({"action_lamp_on", "device_office_lamp", {}});
            check(!result.success && (result.error == "timeout" || result.error == "connection_failure"), "timeout");
            check(executor.production().device("device_office_lamp")->connectivity == execution::Connectivity::Unreachable, "timeout Unreachable");
        }
        {
            auto value = baseV2(root); addPowerAction(value);
            value["bindings"] = Json::array({binding("http://127.0.0.1:1/closed")});
            execution::BoundDeviceExecutor executor(config::ConfigLoader::fromJson(value.dump()));
            const auto result = executor.execute({"action_lamp_on", "device_office_lamp", {}});
            check(!result.success && (result.error == "connection_failure" || result.error == "timeout"), "connection failure");
            check(executor.production().device("device_office_lamp")->connectivity == execution::Connectivity::Unreachable, "connection Unreachable");
        }
        {
            // --- Required intent separation regressions ---
            // 1) Production + empty bindings → binding_missing, no state, no mock success.
            {
                auto value = baseV2(root); addPowerAction(value);
                Model model;
                admin::RuntimeGenerations generations(model);
                generations.apply(config::ConfigLoader::fromJson(value.dump()));
                const auto before = generations.current()->executor->states();
                const auto result = generations.process("Включи лампу", execution::ExecutionIntent::Production);
                // May be Unsupported if alias grounding fails; force via direct production execute too.
                const auto direct = generations.current()->executor->execute(
                    {"action_lamp_on", "device_office_lamp", {}}, execution::ExecutionIntent::Production);
                check(!direct.success && direct.error == "binding_missing", "1 production empty bindings → binding_missing");
                check(std::get<execution::TypedState>(generations.current()->executor->states().back().state).at("power") ==
                    actions::Scalar(false), "1 production empty bindings no state change");
                check(generations.current()->executor->production().device("device_office_lamp")->connectivity ==
                    execution::Connectivity::Unknown, "1 binding_missing leaves connectivity unchanged");
                (void)result; (void)before;
            }
            // 2) Production + valid HTTP binding → fake device receives request.
            {
                FakeHttpServer server;
                auto value = baseV2(root); addPowerAction(value);
                value["bindings"] = Json::array({binding("http://127.0.0.1:" + std::to_string(server.port) + "/gpio/led")});
                Model model;
                admin::RuntimeGenerations generations(model);
                generations.apply(config::ConfigLoader::fromJson(value.dump()));
                const auto result = generations.current()->executor->execute(
                    {"action_lamp_on", "device_office_lamp", {}}, execution::ExecutionIntent::Production);
                check(result.success, "2 production binding success");
                check(server.lastMethod == "POST" && server.lastPath == "/gpio/led", "2 production HTTP received");
                check(generations.current()->executor->production().device("device_office_lamp")->properties.at("power").knowledge ==
                    execution::StateKnowledge::Predicted, "2 production Predicted");
            }
            // 3) Admin playground + empty bindings → virtual Mock execution works.
            {
                auto value = baseV2(root); addPowerAction(value);
                Model model;
                admin::RuntimeGenerations generations(model);
                generations.apply(config::ConfigLoader::fromJson(value.dump()));
                const auto result = generations.current()->executor->execute(
                    {"action_lamp_on", "device_office_lamp", {}}, execution::ExecutionIntent::Playground);
                check(result.success, "3 playground empty bindings mock success");
                check(std::get<execution::TypedState>(generations.current()->executor->states().back().state).at("power") ==
                    actions::Scalar(true), "3 playground mutates virtual state");
                check(generations.current()->executor->production().device("device_office_lamp")->properties.at("power").knowledge ==
                    execution::StateKnowledge::Unknown, "3 playground leaves production Unknown");
            }
            // 4) Admin playground + valid real binding → still Mock; fake HTTP gets nothing.
            {
                FakeHttpServer server;
                auto value = baseV2(root); addPowerAction(value);
                value["bindings"] = Json::array({binding("http://127.0.0.1:" + std::to_string(server.port) + "/relay")});
                Model model;
                admin::RuntimeGenerations generations(model);
                generations.apply(config::ConfigLoader::fromJson(value.dump()));
                const auto result = generations.current()->executor->execute(
                    {"action_lamp_on", "device_office_lamp", {}}, execution::ExecutionIntent::Playground);
                check(result.success, "4 playground with binding still mock success");
                check(server.lastMethod.empty() && server.lastBody.empty(), "4 playground issues no HTTP");
                check(std::get<execution::TypedState>(generations.current()->executor->states().back().state).at("power") ==
                    actions::Scalar(true), "4 playground virtual state only");
                check(generations.current()->executor->production().device("device_office_lamp")->connectivity ==
                    execution::Connectivity::Unknown, "4 playground leaves connectivity Unknown");
            }
        }
        {
            // Scalar argument forwarding for brightness-style actions (Production).
            FakeHttpServer server;
            auto value = baseV2(root);
            value["bindings"] = Json::array({
                {{"target_id", "device_kitchen_light"}, {"action_id", "action_set_brightness"}, {"executor", "http"},
                 {"http", {{"method", "POST"}, {"url", "http://127.0.0.1:" + std::to_string(server.port) + "/brightness"},
                     {"timeout_ms", 1000}, {"content_type", "application/json"}, {"body", Json::object()},
                     {"argument", "brightness"}}}}
            });
            const auto catalog = config::ConfigLoader::fromJson(value.dump());
            execution::BoundDeviceExecutor executor(catalog);
            const auto result = executor.execute({"action_set_brightness", "device_kitchen_light",
                {{"brightness", actions::IntegerArgument(std::int64_t{40})}}}, execution::ExecutionIntent::Production);
            check(result.success, "brightness binding success");
            check(server.lastBody.find("\"brightness\":40") != std::string::npos, "scalar argument forwarded");
            check(executor.production().device("device_kitchen_light")->properties.at("brightness").knowledge ==
                execution::StateKnowledge::Predicted, "brightness Predicted");
        }
        {
            // DNS hostnames rejected at validation (M14 numeric IPv4 only).
            auto value = baseV2(root); addPowerAction(value);
            value["bindings"] = Json::array({binding("http://esp32.local/relay")});
            try { (void)config::ConfigLoader::fromJson(value.dump()); check(false, "hostname url"); }
            catch (const config::ConfigError& error) { check(error.code == "malformed_binding", "hostname url code"); }
        }
        {
            // SafetyGate runs in CommandEngine before any executor intent. A reject
            // must not issue HTTP even when a binding exists and Production is requested.
            FakeHttpServer server;
            auto value = baseV2(root);
            value["bindings"] = Json::array({
                {{"target_id", "device_kitchen_light"}, {"action_id", "action_set_brightness"}, {"executor", "http"},
                 {"http", {{"method", "POST"}, {"url", "http://127.0.0.1:" + std::to_string(server.port) + "/brightness"},
                     {"timeout_ms", 1000}, {"content_type", "application/json"}, {"body", Json::object()},
                     {"argument", "brightness"}}}}
            });
            Model model;
            model.raw = light("device_kitchen_light", 50).dump();
            admin::RuntimeGenerations generations(model);
            generations.apply(config::ConfigLoader::fromJson(value.dump()));
            for (const auto* text : {"Открой дверь", "На кухне выключи свет на 50 процентов"}) {
                server.lastMethod.clear();
                server.lastPath.clear();
                server.lastBody.clear();
                const auto result = generations.process(text, execution::ExecutionIntent::Production);
                check(std::holds_alternative<actions::Unsupported>(result.command.action), "safety rejects candidate");
                check(!result.execution, "safety reject skips executor");
                check(server.lastMethod.empty() && server.lastBody.empty(), "safety reject issues no HTTP");
            }
        }

        // v1 still rejects nonempty bindings at schema boundary
        {
            auto value = base(root);
            value["bindings"] = Json::array({{{"driver", "gpio"}}});
            try { (void)config::ConfigLoader::fromJson(value.dump()); check(false, "v1 bindings empty"); }
            catch (const config::ConfigError& error) { check(error.code == "schema_error", "v1 bindings schema"); }
        }

        std::cout << "PASS " << checks.load() << " hardware binding / HTTP executor checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks.load() << " checks: " << error.what() << "\n";
        return 1;
    }
}
