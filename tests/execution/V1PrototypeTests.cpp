#include "../admin/Support.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/config/ConfigValidator.h"
#include "firefly/execution/ActionExecutor.h"
#include "firefly/execution/DeviceStateStore.h"
#include <atomic>
#include <arpa/inet.h>
#include <cstring>
#include <fstream>
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
int checks = 0;
void check(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}

// Same FakeHttpServer pattern as HardwareBindingTests — existing HttpHardwareExecutor sink.
struct FakeHttpServer {
    int port = 0;
    std::atomic<bool> running{true};
    std::string lastBody;
    int status = 200;
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
            std::string bytes;
            while (bytes.find("\r\n\r\n") == std::string::npos && bytes.size() < 65536) {
                char buffer[4096];
                const auto count = ::recv(client, buffer, sizeof(buffer), 0);
                if (count <= 0) break;
                bytes.append(buffer, static_cast<std::size_t>(count));
            }
            const auto headerEnd = bytes.find("\r\n\r\n");
            if (headerEnd != std::string::npos) lastBody = bytes.substr(headerEnd + 4);
            const std::string reply = "HTTP/1.1 " + std::to_string(status) +
                " OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
            ::send(client, reply.data(), reply.size(), MSG_NOSIGNAL);
            ::close(client);
        }
    }
};

Json demoConfig(const std::string& root, int mockPort) {
    std::ifstream file(root + "/config/demo/v1-kitchen.json");
    auto value = Json::parse(file);
    value["bindings"][0]["http"]["url"] =
        "http://127.0.0.1:" + std::to_string(mockPort) + "/brightness";
    return value;
}

config::Catalog loadDemo(const Json& value) {
    return config::ConfigValidator::validate(
        config::ConfigLoader::fromJson(value.dump())->configuration());
}

actions::GenericAction requireAction(const actions::CandidateResult& value) {
    const auto* action = std::get_if<actions::GenericAction>(&value);
    if (!action) throw std::runtime_error("expected GenericAction");
    return *action;
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        const auto root = argv[1];

        FakeHttpServer server;
        const auto demo = demoConfig(root, server.port);
        const auto snap = loadDemo(demo);

        // --- Voice/text → GenericAction (STT stand-in: deterministic digit phrases) ---
        {
            Model model;
            admin::RuntimeGenerations generations(model);
            generations.apply(snap);
            struct Case { const char* text; int brightness; };
            for (const Case entry : {
                     Case{"Включи свет на кухне", 100},
                     Case{"Выключи свет на кухне", 0},
                     Case{"Поставь свет на кухне на 40 процентов", 40},
                     Case{"на кухне поставь свет на 40 процентов", 40},
                 }) {
                const auto result = generations.process(entry.text, execution::ExecutionIntent::None);
                check(result.command.path == CommandPath::FastPath, "kitchen phrases use fast path");
                const auto action = requireAction(result.command.action);
                check(action.actionId() == "action_set_brightness", "action_set_brightness");
                check(action.targetId() == "device_kitchen_light", "device_kitchen_light");
                check(std::get<std::int64_t>(action.arguments().at("brightness").data()) == entry.brightness,
                    entry.text);
            }
            // Word form “сорок” is not in DeterministicParser (digits only) — LLM fallback on live voice.
            const auto word = generations.process("на кухне поставь свет на сорок процентов");
            check(word.command.path != CommandPath::FastPath, "сорок is not deterministic fast-path");
        }

        // --- Canonical demo scenario (kitchen 40) ---
        {
            Model model;
            admin::RuntimeWorker worker(model);
            admin::LocalRuntimeClient client(worker);
            Directory dir;
            config::ConfigStore store(dir.path);
            store.initialize(snap);
            admin::Api api(store, client);
            api.initializeRuntime();
            const char* text = "Поставь свет на кухне на 40 процентов";
            const auto reply = Json::parse(api.handle("POST", "/api/test-command",
                Json{{"text", text}, {"execute", true}, {"production", true}}.dump()).body);
            check(reply.at("execution").at("status") == "accepted", "canonical demo Accepted");
            check(reply.at("execution").at("current").at("brightness") == 40, "canonical demo brightness 40");
            for (const auto& device : Json::parse(api.handle("GET", "/api/devices/state", "").body).at("devices")) {
                if (device.at("targetId") != "device_kitchen_light") continue;
                check(device.at("properties").at("brightness").at("knowledge") == "predicted", "canonical Predicted");
                check(device.at("properties").at("brightness").at("value") == 40, "canonical value 40");
            }
            std::cout << "DEMO_SCENARIO\n"
                      << "  Command: " << text << '\n'
                      << "  Device: Kitchen Light\n"
                      << "  Result: Accepted\n"
                      << "  State: Ожидается: brightness 40\n";
        }

        // --- Production success → Accepted + Predicted ---
        {
            Model model;
            admin::RuntimeWorker worker(model);
            admin::LocalRuntimeClient client(worker);
            Directory dir;
            config::ConfigStore store(dir.path);
            store.initialize(snap);
            admin::Api api(store, client);
            api.initializeRuntime();

            for (const auto& [text, brightness] : {
                     std::pair<const char*, int>{"Включи свет на кухне", 100},
                     {"Выключи свет на кухне", 0},
                     {"Поставь свет на кухне на 40 процентов", 40},
                 }) {
                const auto reply = Json::parse(api.handle("POST", "/api/test-command",
                    Json{{"text", text}, {"execute", true}, {"production", true}}.dump()).body);
                check(reply.at("ok") == true, "production command ok");
                check(reply.at("result").at("arguments").at("brightness") == brightness, "brightness arg");
                check(reply.at("execution").at("success") == true, "execution success");
                check(reply.at("execution").at("status") == "accepted", "Accepted");
                check(reply.at("execution").at("current").at("brightness") == brightness, "execution current");

                const auto states = Json::parse(api.handle("GET", "/api/devices/state", "").body);
                bool found = false;
                for (const auto& device : states.at("devices")) {
                    if (device.at("targetId") != "device_kitchen_light") continue;
                    found = true;
                    check(device.at("properties").at("brightness").at("knowledge") == "predicted",
                        "Predicted after Accepted");
                    check(device.at("properties").at("brightness").at("value") == brightness,
                        "Predicted brightness value");
                }
                check(found, "kitchen light in production store");
            }
            check(!server.lastBody.empty(), "mock HTTP received body");
        }

        // --- binding_missing → Failed, no Predicted mutation ---
        {
            auto unbound = demo;
            unbound["bindings"] = Json::array();
            const auto unboundSnap = loadDemo(unbound);
            Model model;
            admin::RuntimeWorker worker(model);
            admin::LocalRuntimeClient client(worker);
            Directory dir;
            config::ConfigStore store(dir.path);
            store.initialize(unboundSnap);
            admin::Api api(store, client);
            api.initializeRuntime();

            const auto before = Json::parse(api.handle("GET", "/api/devices/state", "").body);
            std::string beforeKnowledge = "unknown";
            for (const auto& device : before.at("devices"))
                if (device.at("targetId") == "device_kitchen_light")
                    beforeKnowledge = device.at("properties").at("brightness").at("knowledge");

            const auto reply = Json::parse(api.handle("POST", "/api/test-command",
                Json{{"text", "Включи свет на кухне"}, {"execute", true}, {"production", true}}.dump()).body);
            check(reply.at("execution").at("success") == false, "missing binding fails");
            check(reply.at("execution").at("status") == "failed", "Failed status");
            check(reply.at("execution").at("error") == "binding_missing", "binding_missing");

            const auto after = Json::parse(api.handle("GET", "/api/devices/state", "").body);
            for (const auto& device : after.at("devices")) {
                if (device.at("targetId") != "device_kitchen_light") continue;
                check(device.at("properties").at("brightness").at("knowledge") == beforeKnowledge,
                    "no state mutation on binding_missing");
            }
        }

        // --- Playground default unchanged (no production flag) ---
        {
            Model model;
            admin::RuntimeWorker worker(model);
            admin::LocalRuntimeClient client(worker);
            Directory dir;
            config::ConfigStore store(dir.path);
            store.initialize(snap);
            admin::Api api(store, client);
            api.initializeRuntime();
            const auto reply = Json::parse(api.handle("POST", "/api/test-command",
                Json{{"text", "Включи свет на кухне"}, {"execute", true}}.dump()).body);
            check(reply.at("execution").at("success") == true, "playground still works without binding attempt");
            const auto states = Json::parse(api.handle("GET", "/api/devices/state", "").body);
            for (const auto& device : states.at("devices"))
                if (device.at("targetId") == "device_kitchen_light")
                    check(device.at("properties").at("brightness").at("knowledge") == "unknown",
                        "playground does not write Predicted");
        }

        std::cout << "PASS " << checks << " V1 prototype pipeline checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
