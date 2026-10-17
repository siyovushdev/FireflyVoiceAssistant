// Isolated live-Mosquitto acceptance smoke for G1. Requires FIREFLY_ENABLE_MQTT=ON.
// The Python fixture runs a dedicated broker on 127.0.0.1 with a private config.
#include "firefly/config/ConfigLoader.h"
#include "firefly/config/ConfigValidator.h"
#include "firefly/execution/ActionExecutor.h"
#include "firefly/execution/DeviceStateStore.h"
#include <mosquitto.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using Json = nlohmann::json;
using namespace firefly;
namespace {
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
template<class Test>
bool eventually(Test&& test, int milliseconds = 4000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (test()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return test();
}

struct Relay {
    mosquitto* mqtt = nullptr;
    bool running = false;
    std::mutex mutex;
    std::condition_variable ready;
    bool subscribed = false;
    int published = 0;
    std::vector<std::string> commands;
    static void onSubscribe(mosquitto*, void* pointer, int, int count, const int* granted) {
        auto& relay = *static_cast<Relay*>(pointer);
        {
            std::lock_guard<std::mutex> lock(relay.mutex);
            relay.subscribed = count == 1 && granted && granted[0] <= 1 && granted[0] >= 0;
        }
        relay.ready.notify_all();
    }
    static void onPublished(mosquitto*, void* pointer, int) {
        auto& relay = *static_cast<Relay*>(pointer);
        std::lock_guard<std::mutex> lock(relay.mutex);
        ++relay.published;
        relay.ready.notify_all();
    }
    static void onMessage(mosquitto* client, void* pointer, const mosquitto_message* message) {
        auto& relay = *static_cast<Relay*>(pointer);
        if (!message || !message->topic || std::string(message->topic) != "g1/relay/set" ||
            !message->payload || (message->payloadlen != 2 &&
                message->payloadlen != 3)) return;
        std::string payload(static_cast<const char*>(message->payload),
                            static_cast<std::size_t>(message->payloadlen));
        if (payload != "ON" && payload != "OFF") return;
        {
            std::lock_guard<std::mutex> lock(relay.mutex);
            relay.commands.push_back(payload);
        }
        // The simulated device, not Firefly's publish callback, sends actual
        // state telemetry. Only this separate message may create Confirmed.
        (void)mosquitto_publish(client, nullptr, "g1/relay/state",
                                static_cast<int>(payload.size()), payload.data(), 1, false);
    }
    explicit Relay(int port) {
        mqtt = mosquitto_new(nullptr, true, this);
        if (!mqtt) throw std::runtime_error("relay allocation");
        mosquitto_subscribe_callback_set(mqtt, onSubscribe);
        mosquitto_publish_callback_set(mqtt, onPublished);
        mosquitto_message_callback_set(mqtt, onMessage);
        check(mosquitto_connect(mqtt, "127.0.0.1", port, 10) == MOSQ_ERR_SUCCESS,
              "relay cannot connect to isolated broker");
        check(mosquitto_loop_start(mqtt) == MOSQ_ERR_SUCCESS,
              "relay loop cannot start");
        running = true;
        check(mosquitto_subscribe(mqtt, nullptr, "g1/relay/set", 1) == MOSQ_ERR_SUCCESS,
              "relay subscribe request failed");
        std::unique_lock<std::mutex> lock(mutex);
        check(ready.wait_for(lock, std::chrono::seconds(4), [&] { return subscribed; }),
              "relay did not receive accepted SUBACK");
    }
    Relay(const Relay&) = delete;
    Relay& operator=(const Relay&) = delete;
    ~Relay() {
        if (mqtt) {
            if (running) {
                (void)mosquitto_disconnect(mqtt);
                (void)mosquitto_loop_stop(mqtt, true);
            }
            mosquitto_destroy(mqtt);
        }
    }
    void sendState(std::string_view state, bool retain) {
        const int result = mosquitto_publish(mqtt, nullptr, "g1/relay/state",
                            static_cast<int>(state.size()), state.data(), 1, retain);
        check(result == MOSQ_ERR_SUCCESS, "relay state publication failed");
    }
    int commandCount() {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<int>(commands.size());
    }
    std::string command(int index) {
        std::lock_guard<std::mutex> lock(mutex);
        return commands.at(static_cast<std::size_t>(index));
    }
};

Json addDeviceAndBindings(Json root, int port) {
    root["actions"].push_back({
        {"id","action_g1_on"}, {"name","Relay ON"}, {"operation","mock.assign.v1"},
        {"description","Turns on the isolated test relay"},
        {"enabled",true}, {"parameters",Json::array()},
        {"speech_aliases",Json::array({"включи g1 реле"})},
        {"assignments",Json::array({Json{{"field","power"},{"value",true}}})}
    });
    root["actions"].push_back({
        {"id","action_g1_off"}, {"name","Relay OFF"}, {"operation","mock.assign.v1"},
        {"description","Turns off the isolated test relay"},
        {"enabled",true}, {"parameters",Json::array()},
        {"speech_aliases",Json::array({"выключи g1 реле"})},
        {"assignments",Json::array({Json{{"field","power"},{"value",false}}})}
    });
    root["devices"].push_back({
        {"id","device_g1_relay"}, {"name","Test relay"}, {"kind","generic"},
        {"room_id","room_kitchen"}, {"enabled",true},
        {"spoken_aliases",Json::array({"g1 реле"})},
        {"action_ids",Json::array({"action_g1_on","action_g1_off"})},
        {"state",Json::array({Json{
            {"id","power"}, {"name","Power"}, {"type","boolean"},
            {"required",true}, {"description",""}, {"unit",""}, {"initial",false}
        }})},
        {"metadata",Json{{"type","light"},{"description",""},{"aliases",Json::array()}}}
    });
    root["connections"] = Json::array({Json{
        {"id","connection_g1"}, {"name","Isolated broker"}, {"type","mqtt"},
        {"enabled",true}, {"timeout_ms",3000},
        {"network",Json{{"host","127.0.0.1"},{"port",port}}},
        {"mqtt",Json{{"tls",false},{"credentials_id",""}}}
    }});
    root["bindings"] = Json::array({
        Json{
            {"target_id","device_g1_relay"},{"action_id","action_g1_on"},
            {"executor","mqtt"},{"connection_id","connection_g1"},
            {"mqtt",Json{{"topic","g1/relay/set"},{"payload","ON"},{"qos",1},{"retain",false}}}
        },
        Json{
            {"target_id","device_g1_relay"},{"action_id","action_g1_off"},
            {"executor","mqtt"},{"connection_id","connection_g1"},
            {"mqtt",Json{{"topic","g1/relay/set"},{"payload","OFF"},{"qos",0},{"retain",false}}}
        }
    });
    root["observations"] = Json::array({Json{
        {"target_id","device_g1_relay"},{"connection_id","connection_g1"},
        {"executor","mqtt"},
        {"mqtt",Json{{"topic","g1/relay/state"},{"format","text"},{"field","power"},
                     {"true_payload","ON"},{"false_payload","OFF"},{"qos",1}}},
        {"fields",Json::object()}
    }});
    return root;
}

bool confirmed(execution::BoundDeviceExecutor& executor, bool value) {
    const auto state = executor.production().device("device_g1_relay");
    if (!state) return false;
    const auto property = state->properties.find("power");
    return property != state->properties.end() &&
           property->second.knowledge == execution::StateKnowledge::Confirmed &&
           property->second.value &&
           *property->second.value == actions::Scalar(value);
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3) return 2;
        const int port = std::stoi(argv[2]);
        check(port > 0 && port <= 65535, "invalid isolated broker port");
        const auto baseline = config::ConfigLoader::fromFile(
            std::string(argv[1]) + "/config/current-m4.json");
        const auto migrated = config::ConfigValidator::validate(
            config::migrateV2(baseline->configuration()));
        const auto proposed = addDeviceAndBindings(Json::parse(migrated->canonicalJson()), port);
        const auto catalog = config::ConfigLoader::fromJson(proposed.dump());
        // Initialise libmosquitto through Firefly before creating the test
        // relay client. Never call library init twice from competing threads.
        execution::BoundDeviceExecutor executor(catalog);
        Relay relay(port);

        // Retained initial state must not make an unproven device Confirmed.
        relay.sendState("ON", true);
        check(eventually([&] {
            std::lock_guard<std::mutex> lock(relay.mutex);
            return relay.published >= 1;
        }), "retained state did not reach test broker");

        check(!confirmed(executor, true), "state cannot be Confirmed before Apply");
        const auto on = actions::GenericAction("action_g1_on","device_g1_relay",{});
        const auto off = actions::GenericAction("action_g1_off","device_g1_relay",{});
        check(executor.preview(on).success, "preview action valid");
        check(relay.commandCount() == 0, "preview must not publish");
        executor.startSubscriptions();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        check(!confirmed(executor, true), "retained MQTT payload must not confirm state");

        // Send independent fresh telemetry until SUBACK/observer are ready.
        int probes = 0;
        check(eventually([&] {
            if ((probes++ % 10) == 0) relay.sendState("OFF", false);
            return confirmed(executor, false);
        }), "fresh state topic never became Confirmed");

        check(executor.execute(on).success, "ON publish failed");
        check(eventually([&] { return relay.commandCount() == 1; }),
              "ON not received by the simulated relay");
        check(relay.command(0) == "ON", "wrong ON payload or topic");
        check(eventually([&] { return confirmed(executor, true); }),
              "ON state was not separately confirmed");

        check(executor.execute(off).success, "OFF QoS0 send failed");
        check(eventually([&] { return relay.commandCount() == 2; }),
              "OFF not received by simulated relay");
        check(relay.command(1) == "OFF", "wrong OFF payload or topic");
        check(eventually([&] { return confirmed(executor, false); }),
              "OFF state was not separately confirmed");

        check(!executor.execute(actions::GenericAction(
                 "action_g1_on", "nonexistent_device", {})).success,
              "wrong target cannot publish");
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        check(relay.commandCount() == 2, "unexpected command replay or extra publish");
        std::cout << "PASS isolated MQTT 3.1.1 broker ON QoS1 / OFF QoS0, "
                  << "state subscription, retained ignored, no extra publishes\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "MQTT integration FAIL: " << e.what() << '\n';
        return 1;
    }
}
