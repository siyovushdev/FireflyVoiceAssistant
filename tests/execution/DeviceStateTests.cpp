#include "../admin/Support.h"
#include "firefly/execution/ActionExecutor.h"
#include "firefly/execution/DeviceStateStore.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/config/ConfigValidator.h"
#include <atomic>
#include <arpa/inet.h>
#include <chrono>
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
    std::string lastMethod, lastPath, lastBody;
    int status = 200;
    int delayMs = 0;
    std::thread thread;
    int listenFd = -1;
    explicit FakeHttpServer(int responseStatus = 200) : status(responseStatus) {
        listenFd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listenFd < 0) throw std::runtime_error("socket");
        int on = 1; ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = 0;
        if (::bind(listenFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) throw std::runtime_error("bind");
        socklen_t length = sizeof(address);
        if (::getsockname(listenFd, reinterpret_cast<sockaddr*>(&address), &length) != 0) throw std::runtime_error("getsockname");
        port = ntohs(address.sin_port);
        if (::listen(listenFd, 8) != 0) throw std::runtime_error("listen");
        thread = std::thread([this] {
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
                }
                const std::string reply = "HTTP/1.1 " + std::to_string(status) + " OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
                ::send(client, reply.data(), reply.size(), MSG_NOSIGNAL);
                ::close(client);
            }
        });
    }
    ~FakeHttpServer() {
        running = false;
        if (listenFd >= 0) ::shutdown(listenFd, SHUT_RDWR);
        if (thread.joinable()) thread.join();
        if (listenFd >= 0) ::close(listenFd);
    }
};

Json baseV2(const std::string& root) {
    auto value = base(root);
    return Json::parse(config::ConfigValidator::validate(
        config::migrateV2(config::ConfigLoader::fromJson(value.dump())->configuration()))->canonicalJson());
}
std::int64_t brightness(const std::vector<execution::DeviceRecord>& states, std::string id = "device_kitchen_light") {
    for (const auto& device : states) if (device.targetId == id) return std::get<execution::LightState>(device.state).brightness;
    throw std::runtime_error("missing test device");
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        const auto root = argv[1];
        std::int64_t clockMs = 1'000'000;
        auto clock = [&] { return clockMs; };

        // --- Initial Unknown ---
        {
            auto value = baseV2(root);
            execution::BoundDeviceExecutor executor(config::ConfigLoader::fromJson(value.dump()), clock);
            const auto kitchen = executor.production().device("device_kitchen_light");
            check(kitchen && kitchen->properties.at("brightness").knowledge == execution::StateKnowledge::Unknown, "initial Unknown");
            check(!kitchen->properties.at("brightness").value, "initial no fabricated value");
            check(kitchen->connectivity == execution::Connectivity::Unknown, "initial connectivity Unknown");
        }

        // --- Execution Accepted / Failed + Prediction ---
        {
            FakeHttpServer server;
            auto value = baseV2(root);
            value["bindings"] = Json::array({{
                {"target_id", "device_kitchen_light"}, {"action_id", "action_set_brightness"}, {"executor", "http"},
                {"http", {{"method", "POST"}, {"url", "http://127.0.0.1:" + std::to_string(server.port) + "/b"},
                    {"timeout_ms", 1000}, {"content_type", "application/json"}, {"body", Json::object()},
                    {"argument", "brightness"}}}}});
            execution::BoundDeviceExecutor executor(config::ConfigLoader::fromJson(value.dump()), clock);
            const auto accepted = executor.execute(
                {"action_set_brightness", "device_kitchen_light", {{"brightness", actions::IntegerArgument(std::int64_t{40})}}},
                execution::ExecutionIntent::Production);
            check(accepted.success && accepted.status == execution::ExecutionStatus::Accepted, "Accepted");
            auto knowledge = executor.production().device("device_kitchen_light");
            check(knowledge->properties.at("brightness").knowledge == execution::StateKnowledge::Predicted, "Predicted after accept");
            check(knowledge->properties.at("brightness").value == actions::Scalar(std::int64_t{40}), "Predicted value 40");
            check(knowledge->connectivity == execution::Connectivity::Reachable, "Reachable after accept");

            FakeHttpServer fail(500);
            value["bindings"][0]["http"]["url"] = "http://127.0.0.1:" + std::to_string(fail.port) + "/fail";
            execution::BoundDeviceExecutor failed(config::ConfigLoader::fromJson(value.dump()), clock);
            // Seed prediction then fail a different command path — fresh executor starts Unknown.
            const auto failure = failed.execute(
                {"action_set_brightness", "device_kitchen_light", {{"brightness", actions::IntegerArgument(std::int64_t{50})}}},
                execution::ExecutionIntent::Production);
            check(!failure.success && failure.status == execution::ExecutionStatus::Failed, "Failed");
            check(failure.error.find("http_error") != std::string::npos, "Failed is http_error");
            check(failed.production().device("device_kitchen_light")->properties.at("brightness").knowledge ==
                execution::StateKnowledge::Unknown, "failed execution no prediction");
            check(failed.production().device("device_kitchen_light")->connectivity ==
                execution::Connectivity::Reachable, "HTTP 500 → Execution Failed + Connectivity Reachable");
        }

        // --- Connectivity evidence isolation (Failed ≠ Unreachable) ---
        {
            auto value = baseV2(root);
            value["bindings"] = Json::array({{
                {"target_id", "device_kitchen_light"}, {"action_id", "action_set_brightness"}, {"executor", "http"},
                {"http", {{"method", "POST"}, {"url", "http://127.0.0.1:1/closed"},
                    {"timeout_ms", 200}, {"content_type", "application/json"}, {"body", Json::object()},
                    {"argument", "brightness"}}}}});
            execution::BoundDeviceExecutor unreachable(config::ConfigLoader::fromJson(value.dump()), clock);
            const auto failure = unreachable.execute(
                {"action_set_brightness", "device_kitchen_light", {{"brightness", actions::IntegerArgument(std::int64_t{40})}}},
                execution::ExecutionIntent::Production);
            check(!failure.success && failure.status == execution::ExecutionStatus::Failed, "connection Failed");
            check(unreachable.production().device("device_kitchen_light")->connectivity ==
                execution::Connectivity::Unreachable, "connection failure → Unreachable");

            // malformed / binding_missing → connectivity unchanged
            value["bindings"] = Json::array();
            execution::BoundDeviceExecutor missing(config::ConfigLoader::fromJson(value.dump()), clock);
            const auto before = missing.production().device("device_kitchen_light")->connectivity;
            const auto noBinding = missing.execute(
                {"action_set_brightness", "device_kitchen_light", {{"brightness", actions::IntegerArgument(std::int64_t{40})}}},
                execution::ExecutionIntent::Production);
            check(!noBinding.success && noBinding.error == "binding_missing", "binding_missing Failed");
            check(missing.production().device("device_kitchen_light")->connectivity == before,
                "binding_missing leaves connectivity unchanged");
        }

        // --- Confirmation + conflict ---
        {
            FakeHttpServer server;
            auto value = baseV2(root);
            value["bindings"] = Json::array({{
                {"target_id", "device_kitchen_light"}, {"action_id", "action_set_brightness"}, {"executor", "http"},
                {"http", {{"method", "POST"}, {"url", "http://127.0.0.1:" + std::to_string(server.port) + "/b"},
                    {"timeout_ms", 1000}, {"content_type", "application/json"}, {"body", Json::object()},
                    {"argument", "brightness"}}}}});
            execution::BoundDeviceExecutor executor(config::ConfigLoader::fromJson(value.dump()), clock);
            executor.execute({"action_set_brightness", "device_kitchen_light",
                {{"brightness", actions::IntegerArgument(std::int64_t{40})}}}, execution::ExecutionIntent::Production);
            clockMs += 1000;
            executor.observe(execution::StateObservation{
                "device_kitchen_light", {{"brightness", actions::Scalar(std::int64_t{40})}}, clockMs});
            auto knowledge = executor.production().device("device_kitchen_light");
            check(knowledge->properties.at("brightness").knowledge == execution::StateKnowledge::Confirmed, "Confirmed match");
            check(knowledge->properties.at("brightness").value == actions::Scalar(std::int64_t{40}), "Confirmed value");

            clockMs += 1000;
            executor.observe(execution::StateObservation{
                "device_kitchen_light", {{"brightness", actions::Scalar(std::int64_t{0})}}, clockMs});
            knowledge = executor.production().device("device_kitchen_light");
            check(knowledge->properties.at("brightness").knowledge == execution::StateKnowledge::Confirmed, "conflict Confirmed");
            check(knowledge->properties.at("brightness").value == actions::Scalar(std::int64_t{0}), "observation wins OFF");
        }

        // --- Out-of-order observation protection ---
        {
            auto value = baseV2(root);
            execution::DeviceStateStore store(config::ConfigLoader::fromJson(value.dump()), clock);
            store.applyPrediction("device_kitchen_light", {{"brightness", actions::Scalar(std::int64_t{40})}}, 2000);
            try {
                store.applyObservation(execution::StateObservation{
                    "device_kitchen_light", {{"brightness", actions::Scalar(std::int64_t{10})}}, 1000});
                check(false, "older observation after prediction must reject");
            } catch (const config::ConfigError& error) {
                check(error.code == "stale_observation", "stale after prediction code");
            }
            check(store.device("device_kitchen_light")->properties.at("brightness").knowledge ==
                execution::StateKnowledge::Predicted, "prediction survives older observation");
            check(store.device("device_kitchen_light")->properties.at("brightness").value ==
                actions::Scalar(std::int64_t{40}), "prediction value unchanged by stale observation");

            store.applyObservation(execution::StateObservation{
                "device_kitchen_light", {{"brightness", actions::Scalar(std::int64_t{40})}}, 3000});
            try {
                store.applyObservation(execution::StateObservation{
                    "device_kitchen_light", {{"brightness", actions::Scalar(std::int64_t{0})}}, 2500});
                check(false, "older observation after confirmed must reject");
            } catch (const config::ConfigError& error) {
                check(error.code == "stale_observation", "stale after confirmed code");
            }
            check(store.device("device_kitchen_light")->properties.at("brightness").knowledge ==
                execution::StateKnowledge::Confirmed, "confirmed survives older observation");
            check(store.device("device_kitchen_light")->properties.at("brightness").value ==
                actions::Scalar(std::int64_t{40}), "confirmed value unchanged by stale observation");

            store.applyObservation(execution::StateObservation{
                "device_kitchen_light", {{"brightness", actions::Scalar(std::int64_t{70})}}, 4000});
            const auto newer = store.device("device_kitchen_light")->properties.at("brightness");
            check(newer.knowledge == execution::StateKnowledge::Confirmed, "newer observation Confirmed");
            check(newer.value == actions::Scalar(std::int64_t{70}), "newer observation value");
            check(newer.observedAtMs == 4000, "newer observation stamp");
        }

        // --- Built-in observation schema validation ---
        {
            auto value = baseV2(root);
            execution::DeviceStateStore store(config::ConfigLoader::fromJson(value.dump()), clock);
            try {
                store.applyObservation(execution::StateObservation{
                    "device_kitchen_light", {{"brightness", actions::Scalar(std::string("bright"))}}, clockMs});
                check(false, "string brightness must reject");
            } catch (const config::ConfigError& error) {
                check(error.code == "unsafe_constraint", "brightness type code");
            }
            try {
                store.applyObservation(execution::StateObservation{
                    "device_kitchen_light", {{"brightness", actions::Scalar(std::int64_t{150})}}, clockMs});
                check(false, "out-of-domain brightness must reject");
            } catch (const config::ConfigError& error) {
                check(error.code == "unsafe_constraint", "brightness domain code");
            }
            check(store.device("device_kitchen_light")->properties.at("brightness").knowledge ==
                execution::StateKnowledge::Unknown, "invalid brightness leaves Unknown");

            try {
                store.applyObservation(execution::StateObservation{
                    "device_entrance_door", {{"position", actions::Scalar(std::string("AJAR"))}}, clockMs});
                check(false, "invalid door position must reject");
            } catch (const config::ConfigError& error) {
                check(error.code == "unsafe_constraint", "door position code");
            }
            try {
                store.applyObservation(execution::StateObservation{
                    "device_entrance_door", {{"position", actions::Scalar(std::int64_t{1})}}, clockMs});
                check(false, "integer door position must reject");
            } catch (const config::ConfigError& error) {
                check(error.code == "unsafe_constraint", "door type code");
            }
            // Contradiction with a prior prediction is allowed; schema still holds.
            store.applyPrediction("device_kitchen_light", {{"brightness", actions::Scalar(std::int64_t{40})}}, clockMs);
            clockMs += 1;
            store.applyObservation(execution::StateObservation{
                "device_kitchen_light", {{"brightness", actions::Scalar(std::int64_t{0})}}, clockMs});
            check(store.device("device_kitchen_light")->properties.at("brightness").value ==
                actions::Scalar(std::int64_t{0}), "observation may contradict prediction");
        }

        // --- Connectivity isolation ---
        {
            auto value = baseV2(root);
            execution::BoundDeviceExecutor executor(config::ConfigLoader::fromJson(value.dump()), clock);
            Model model;
            admin::RuntimeGenerations generations(model);
            generations.apply(config::ConfigLoader::fromJson(value.dump()));
            generations.process("На кухне выключи свет на 50 процентов", execution::ExecutionIntent::Production);
            check(generations.current()->executor->production().device("device_kitchen_light")->connectivity ==
                execution::Connectivity::Unknown, "SafetyGate reject no connectivity change");
            generations.current()->executor->execute(
                {"action_set_brightness", "device_kitchen_light", {{"brightness", actions::IntegerArgument(std::int64_t{40})}}},
                execution::ExecutionIntent::Playground);
            check(generations.current()->executor->production().device("device_kitchen_light")->connectivity ==
                execution::Connectivity::Unknown, "Playground no connectivity change");
        }

        // Generic actions update only their assignments, never the complete
        // Playground preview (which contains initial or simulated values).
        {
            FakeHttpServer server;
            std::ifstream input(std::string(root) + "/tests/language/home.json");
            auto value = Json::parse(input);
            value["devices"][0]["state"].push_back({{"id", "power"}, {"name", "Питание"},
                {"type", "boolean"}, {"initial", false}, {"required", true}, {"unit", ""}, {"description", ""}});
            for (auto& action : value["actions"]) {
                const auto id = action.at("id").get<std::string>();
                if (id == "power_on" || id == "power_off")
                    action["assignments"] = Json::array({{{"field", "power"}, {"value", id == "power_on"}}});
                if (id == "power_off") {
                    action["parameters"] = Json::array({{{"id", "level"}, {"name", "Яркость"},
                        {"type", "integer"}, {"minimum", 0}, {"maximum", 100}, {"required", false},
                        {"unit", "percent"}, {"description", ""}}});
                    action["assignments"].push_back({{"field", "level"}, {"argument", "level"}});
                }
                Json http = {{"method", "POST"}, {"url", "http://127.0.0.1:" + std::to_string(server.port) + "/command"},
                    {"timeout_ms", 1000}, {"body", Json::object()}};
                if (id == "set_level") http["argument"] = "level";
                value["bindings"].push_back({{"target_id", "lamp_a"}, {"action_id", id}, {"executor", "http"}, {"http", http}});
            }
            execution::BoundDeviceExecutor executor(config::ConfigLoader::fromJson(value.dump()), clock);
            auto property = [&](const std::string& key) { return executor.production().device("lamp_a")->properties.at(key); };
            auto unchanged = [](const execution::PropertyKnowledge& a, const execution::PropertyKnowledge& b) {
                return a.value == b.value && a.knowledge == b.knowledge && a.updatedAtMs == b.updatedAtMs && a.observedAtMs == b.observedAtMs;
            };
            auto execute = [&](const actions::GenericAction& action, execution::ExecutionIntent intent) {
                ++clockMs;
                check(executor.execute(action, intent).success, "generic test action accepted by HTTP or Playground");
            };
            const auto unknownPower = property("power");
            execute({"set_level", "lamp_a", {{"level", actions::Scalar(std::int64_t{40})}}}, execution::ExecutionIntent::Production);
            check(unchanged(property("power"), unknownPower), "brightness does not promote untouched power Unknown to Predicted");
            const auto predictedLevel = property("level");
            check(predictedLevel.value == actions::Scalar(std::int64_t{40}) && predictedLevel.knowledge == execution::StateKnowledge::Predicted, "production brightness is Predicted 40");
            execute({"power_on", "lamp_a", {}}, execution::ExecutionIntent::Production);
            check(unchanged(property("level"), predictedLevel), "production ON does not replace brightness with virtual initial zero");
            const auto predictedPower = property("power");
            execute({"set_level", "lamp_a", {{"level", actions::Scalar(std::int64_t{55})}}}, execution::ExecutionIntent::Playground);
            check(unchanged(property("level"), predictedLevel) && unchanged(property("power"), predictedPower), "Playground leaves all production property metadata intact");
            execute({"power_off", "lamp_a", {}}, execution::ExecutionIntent::Production);
            check(unchanged(property("level"), predictedLevel), "production OFF never imports Playground 55, absent optional argument leaves field intact");
            check(property("power").value == actions::Scalar(false), "production OFF updates its literal assignment");
            executor.observe({"lamp_a", {{"level", actions::Scalar(std::int64_t{70})}}, ++clockMs});
            const auto confirmedLevel = property("level");
            execute({"power_on", "lamp_a", {}}, execution::ExecutionIntent::Production);
            check(confirmedLevel.knowledge == execution::StateKnowledge::Confirmed && unchanged(property("level"), confirmedLevel), "unrelated production command preserves Confirmed value and both timestamps");
            execute({"power_off", "lamp_a", {{"level", actions::Scalar(std::int64_t{10})}}}, execution::ExecutionIntent::Production);
            check(property("level").value == actions::Scalar(std::int64_t{10}) && property("level").knowledge == execution::StateKnowledge::Predicted,
                "present optional argument updates its field as Predicted");
        }

        // --- Stale (deterministic clock, no sleep) ---
        {
            auto value = baseV2(root);
            execution::DeviceStateStore store(config::ConfigLoader::fromJson(value.dump()), clock);
            store.applyPrediction("device_kitchen_light", {{"brightness", actions::Scalar(std::int64_t{40})}}, clockMs);
            auto property = store.device("device_kitchen_light")->properties.at("brightness");
            check(!execution::isStale(property, clockMs), "fresh predicted not stale");
            check(execution::isStale(property, clockMs + execution::kDefaultStateTtlMs + 1), "beyond TTL stale");
            clockMs += 500;
            store.applyObservation(execution::StateObservation{
                "device_kitchen_light", {{"brightness", actions::Scalar(std::int64_t{40})}}, clockMs});
            property = store.device("device_kitchen_light")->properties.at("brightness");
            check(!execution::isStale(property, clockMs + 10), "fresh confirmed not stale");
            check(execution::isStale(property, clockMs + execution::kDefaultStateTtlMs + 1), "confirmed beyond TTL stale");
        }

        // --- Generation reconcile ---
        {
            FakeHttpServer server;
            auto value = baseV2(root);
            value["bindings"] = Json::array({{
                {"target_id", "device_kitchen_light"}, {"action_id", "action_set_brightness"}, {"executor", "http"},
                {"http", {{"method", "POST"}, {"url", "http://127.0.0.1:" + std::to_string(server.port) + "/b"},
                    {"timeout_ms", 1000}, {"content_type", "application/json"}, {"body", Json::object()},
                    {"argument", "brightness"}}}}});
            Model model;
            admin::RuntimeGenerations generations(model);
            generations.apply(config::ConfigLoader::fromJson(value.dump()));
            generations.current()->executor->execute(
                {"action_set_brightness", "device_kitchen_light", {{"brightness", actions::IntegerArgument(std::int64_t{40})}}},
                execution::ExecutionIntent::Production);
            // Use the generation executor clock (not the shared test clock) so the
            // observation is not older than the Predicted stamp from execute().
            generations.observe(execution::StateObservation{
                "device_kitchen_light", {{"brightness", actions::Scalar(std::int64_t{40})}},
                generations.current()->executor->production().nowMs()});
            auto next = bathroom(value); next["revision"] = 2;
            // bathroom() helper is v1-oriented; complete v2 required device fields.
            for (auto& device : next["devices"]) {
                if (!device.contains("state")) device["state"] = Json::array();
                if (!device.contains("metadata"))
                    device["metadata"] = {{"type", device.value("kind", "light")}, {"description", ""}, {"aliases", Json::array()}};
            }
            generations.apply(config::ConfigLoader::fromJson(next.dump()));
            const auto& store = generations.current()->executor->production();
            check(store.device("device_kitchen_light")->properties.at("brightness").knowledge ==
                execution::StateKnowledge::Confirmed, "reconcile preserves Confirmed");
            check(store.device("device_bathroom_light")->properties.at("brightness").knowledge ==
                execution::StateKnowledge::Unknown, "new device Unknown");
            auto removed = value; removed["revision"] = 3;
            generations.apply(config::ConfigLoader::fromJson(removed.dump()));
            check(!generations.current()->executor->production().device("device_bathroom_light"), "removed device gone");
        }

        // --- Intent isolation ---
        {
            FakeHttpServer server;
            auto value = baseV2(root);
            value["bindings"] = Json::array({{
                {"target_id", "device_kitchen_light"}, {"action_id", "action_set_brightness"}, {"executor", "http"},
                {"http", {{"method", "POST"}, {"url", "http://127.0.0.1:" + std::to_string(server.port) + "/b"},
                    {"timeout_ms", 1000}, {"content_type", "application/json"}, {"body", Json::object()},
                    {"argument", "brightness"}}}}});
            Model model;
            admin::RuntimeGenerations generations(model);
            generations.apply(config::ConfigLoader::fromJson(value.dump()));
            generations.process("На кухне поставь свет на 40 процентов", execution::ExecutionIntent::Playground);
            check(server.lastMethod.empty(), "playground no HTTP");
            check(generations.current()->executor->production().device("device_kitchen_light")->properties.at("brightness").knowledge ==
                execution::StateKnowledge::Unknown, "playground no production mutation");
            check(brightness(generations.current()->executor->states()) == 40, "playground virtual mutated");
        }

        std::cout << "PASS " << checks.load() << " device state / execution lifecycle checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks.load() << " checks: " << error.what() << "\n";
        return 1;
    }
}
