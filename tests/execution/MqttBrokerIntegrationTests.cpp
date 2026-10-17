// Isolated live-Mosquitto acceptance smoke for G1. Requires FIREFLY_ENABLE_MQTT=ON.
// The Python fixture runs a dedicated broker on 127.0.0.1 with a private config.
#include "firefly/config/ConfigLoader.h"
#include "firefly/config/ConfigValidator.h"
#include "firefly/admin/RuntimeGeneration.h"
#include "firefly/execution/ActionExecutor.h"
#include "firefly/execution/DeviceStateStore.h"
#include <mosquitto.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <condition_variable>
#include <fstream>
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
    static void onConnect(mosquitto* client, void*, int code) noexcept {
        if (code != 0) return;
        // Re-subscribe after clean-session reconnect; never publish commands.
        (void)mosquitto_subscribe(client, nullptr, "g1/relay/set", 1);
    }
    static void onDisconnect(mosquitto*, void* pointer, int) noexcept {
        auto& relay = *static_cast<Relay*>(pointer);
        try {
            std::lock_guard<std::mutex> lock(relay.mutex);
            relay.subscribed = false;
        } catch (const std::exception&) {}
    }
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
        // A constructor that throws does not call its own destructor.
        // Clean up the broker-loop client on every test initialization error.
        try {
            mqtt = mosquitto_new(nullptr, true, this);
            if (!mqtt) throw std::runtime_error("relay allocation");
            mosquitto_connect_callback_set(mqtt, onConnect);
            mosquitto_disconnect_callback_set(mqtt, onDisconnect);
            mosquitto_subscribe_callback_set(mqtt, onSubscribe);
            mosquitto_publish_callback_set(mqtt, onPublished);
            mosquitto_message_callback_set(mqtt, onMessage);
            check(mosquitto_connect(mqtt, "127.0.0.1", port, 10) == MOSQ_ERR_SUCCESS,
                  "relay cannot connect to isolated broker");
            check(mosquitto_loop_start(mqtt) == MOSQ_ERR_SUCCESS,
                  "relay loop cannot start");
            running = true;
            std::unique_lock<std::mutex> lock(mutex);
            check(ready.wait_for(lock, std::chrono::seconds(4), [&] { return subscribed; }),
                  "relay did not receive accepted SUBACK");
        } catch (...) {
            if (mqtt) {
                if (running) {
                    (void)mosquitto_disconnect(mqtt);
                    (void)mosquitto_loop_stop(mqtt, true);
                }
                mosquitto_destroy(mqtt);
                mqtt = nullptr;
            }
            throw;
        }
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
    void sendTopic(std::string_view topic, std::string_view state, bool retain = false) {
        const std::string owned(topic);
        const int result = mosquitto_publish(mqtt, nullptr, owned.c_str(),
                            static_cast<int>(state.size()), state.data(), 1, retain);
        check(result == MOSQ_ERR_SUCCESS, "test relay telemetry publication failed");
    }
    void sendState(std::string_view state, bool retain) {
        sendTopic("g1/relay/state", state, retain);
    }
    void sendJsonState(std::string_view state) {
        const int result = mosquitto_publish(mqtt, nullptr, "g1/json/state",
                            static_cast<int>(state.size()), state.data(), 1, false);
        check(result == MOSQ_ERR_SUCCESS, "JSON state publication failed");
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
    // A second device subscribes to JSON observations on this broker,
    // independent from the relay command/observation state.
    root["devices"].push_back({
        {"id","device_g1_json_probe"}, {"name","JSON state probe"},
        {"kind","generic"}, {"room_id","room_kitchen"}, {"enabled",true},
        {"spoken_aliases",Json::array({"g1 датчик"})},
        {"action_ids",Json::array()},
        {"state",Json::array({Json{
            {"id","power"}, {"name","Power"}, {"type","boolean"},
            {"required",true}, {"description",""}, {"unit",""}, {"initial",false}
        }})},
        {"metadata",Json{{"type","sensor"},{"description",""},{"aliases",Json::array()}}}
    });
    root["connections"] = Json::array({Json{
        {"id","connection_g1"}, {"name","Isolated broker"}, {"type","mqtt"},
        {"enabled",true}, {"timeout_ms",3000},
        {"network",Json{{"host","127.0.0.1"},{"port",port}}},
        {"mqtt",Json{{"tls",false}}}
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
    root["observations"] = Json::array({
        Json{
            {"target_id","device_g1_relay"},{"connection_id","connection_g1"},
            {"executor","mqtt"},
            {"mqtt",Json{{"topic","g1/relay/state"},{"format","text"},{"field","power"},
                         {"true_payload","ON"},{"false_payload","OFF"},{"qos",1}}},
            {"fields",Json::object()}
        },
        Json{
            {"target_id","device_g1_json_probe"},{"connection_id","connection_g1"},
            {"executor","mqtt"},
            {"mqtt",Json{{"topic","g1/json/state"},{"format","json"},{"qos",1}}},
            {"fields",Json{{"power","power"}}}
        }
    });
    return root;
}

bool confirmed(execution::BoundDeviceExecutor& executor, bool value,
               const std::string& target = "device_g1_relay") {
    const auto state = executor.production().device(target);
    if (!state) return false;
    const auto property = state->properties.find("power");
    return property != state->properties.end() &&
           property->second.knowledge == execution::StateKnowledge::Confirmed &&
           property->second.value &&
           *property->second.value == actions::Scalar(value);
}
class LocalModel final : public admin::ContextModel {
public:
    void useContext(const std::string& value) override { prompt_ = value; }
    bool ready() const override { return !prompt_.empty(); }
    std::string infer(std::string_view) override { return R"({"unsupported":true})"; }
private:
    std::string prompt_;
};
bool unknown(execution::BoundDeviceExecutor& executor, const std::string& target) {
    const auto record = executor.production().device(target);
    if (!record) return false;
    const auto found = record->properties.find("power");
    return found != record->properties.end() &&
           found->second.knowledge == execution::StateKnowledge::Unknown;
}
// Real RuntimeGenerations::apply, not merely a direct executor constructor.
// Retaining old immutable snapshots must NOT retain old broker subscriptions.
void testGenerationRetirement(const Json& base, Relay& relay) {
    auto a = base;
    for (auto& o : a["observations"])
        if (o.at("target_id") == "device_g1_relay")
            o["mqtt"]["topic"] = "g1/generations/a";
    LocalModel model;
    admin::RuntimeGenerations runtime(model);
    const auto catalogA = config::ConfigLoader::fromJson(a.dump());
    runtime.apply(catalogA);
    const auto old = runtime.current();
    check(old && old->id != 0, "first live MQTT RuntimeGeneration published");
    int attempts = 0;
    check(eventually([&] {
        if ((attempts++ % 10) == 0) relay.sendTopic("g1/generations/a", "OFF");
        return confirmed(*old->executor, false);
    }), "first generation failed to confirm its subscription");

    auto b = a;
    for (auto& o : b["observations"])
        if (o.at("target_id") == "device_g1_relay")
            o["mqtt"]["topic"] = "g1/generations/b";
    const auto catalogB = config::ConfigLoader::fromJson(b.dump());
    runtime.preflight(catalogB);
    check(runtime.current() == old, "preflight must not retire active MQTT generation");
    runtime.apply(catalogB);
    const auto newer = runtime.current();
    check(newer && newer != old, "Apply published a distinct RuntimeGeneration");
    check(unknown(*newer->executor, "device_g1_relay"),
          "changed MQTT mapping must not inherit old Confirmed knowledge");
    // The old generation is intentionally retained. Its subscriber must be
    // stopped/joined at successful Apply, not delayed until shared_ptr release.
    for (int i = 0; i < 15; ++i)
        relay.sendTopic("g1/generations/a", "ON");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    check(confirmed(*old->executor, false),
          "old generation's held snapshot received obsolete MQTT telemetry");
    check(unknown(*newer->executor, "device_g1_relay"),
          "old topic leaked into new generation after Apply");

    attempts = 0;
    check(eventually([&] {
        if ((attempts++ % 10) == 0) relay.sendTopic("g1/generations/b", "OFF");
        return confirmed(*newer->executor, false);
    }), "new generation failed to subscribe to new state topic");

    // Remove observation entirely, retaining both older generation objects.
    auto without = b;
    Json retained = Json::array();
    for (const auto& o : without["observations"])
        if (o.at("target_id") != "device_g1_relay") retained.push_back(o);
    without["observations"] = std::move(retained);
    runtime.apply(config::ConfigLoader::fromJson(without.dump()));
    const auto removed = runtime.current();
    check(removed && removed != newer, "removal creates another generation");
    check(unknown(*removed->executor, "device_g1_relay"),
          "removed mapping must not retain Confirmed knowledge");
    for (int i = 0; i < 15; ++i)
        relay.sendTopic("g1/generations/b", "ON");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    check(confirmed(*newer->executor, false),
          "previous generation subscriber survived mapping removal");
    check(unknown(*removed->executor, "device_g1_relay"),
          "removed MQTT observation still updated active state");
    check(confirmed(*old->executor, false),
          "first held generation was reactivated by later Apply");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 4 && argc != 5) return 2;
        const bool initialDown = argc == 5 && std::string(argv[4]) == "--initial-offline";
        if (argc == 5 && !initialDown) return 2;
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
        if (initialDown) {
            // A previously unavailable broker must recover without a new
            // Apply/generation or manual second startSubscriptions call.
            // Python starts the isolated broker ONLY after this client has
            // entered Unreachable via its initial failed connection.
            executor.startSubscriptions();
            check(eventually([&] {
                const auto snapshot = executor.production().device("device_g1_relay");
                return snapshot && snapshot->connectivity == execution::Connectivity::Unreachable &&
                    snapshot->properties.at("power").knowledge == execution::StateKnowledge::Unknown;
            }, 4000), "initial offline subscription never reached Unreachable");
            // Repeated calls must not spawn another connection or retry loop.
            executor.startSubscriptions();
            executor.startSubscriptions();
            const std::string directory(argv[3]);
            { std::ofstream signal(directory + "/startup-request");
              check(static_cast<bool>(signal << "start\n"), "cannot request test broker startup"); }
            check(eventually([&] {
                std::ifstream marker(directory + "/broker-ready");
                return marker.good();
            }, 7000), "test broker did not become available");
            Relay relay(port);
            check(!confirmed(executor, true), "reconnect alone must not create Confirmed");
            int attempts = 0;
            check(eventually([&] {
                if ((attempts++ % 10) == 0) relay.sendState("ON", false);
                return confirmed(executor, true);
            }, 14000), "initial offline subscription failed to recover with fresh state");
            check(relay.commandCount() == 0, "subscriber recovery replayed a hardware command");
            std::cout << "PASS initial broker absent -> retry -> subscribe -> fresh Confirmed\n";
            return 0;
        }
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

        // Live strict JSON test: establish the JSON probe's known-good state.
        // Bad messages would change power to true if their extra fields were
        // silently dropped by the decoder.
        int jsonProbes = 0;
        check(eventually([&] {
            if ((jsonProbes++ % 10) == 0) relay.sendJsonState(R"({"power":false})");
            return confirmed(executor, false, "device_g1_json_probe");
        }), "valid JSON state did not become Confirmed");
        for (const auto* invalid : {
                 R"({"power":true,"unmapped":7})",
                 R"({"power":true,"unmapped":{"nested":false}})",
                 R"({"power":true,"unmapped":[1,2]})"
             }) {
            // Same broker and publisher client preserve message order. A
            // separate relay ON then OFF makes a delivery barrier: after the
            // OFF reaches the Firefly subscriber, earlier invalid JSON has
            // already traversed the same subscription callback thread.
            relay.sendState("ON", false);
            check(eventually([&] { return confirmed(executor, true); }),
                  "live JSON rejection barrier ON was not observed");
            relay.sendJsonState(invalid);
            relay.sendState("OFF", false);
            check(eventually([&] { return confirmed(executor, false); }),
                  "live JSON rejection barrier OFF was not observed");
            check(confirmed(executor, false, "device_g1_json_probe"),
                  "unknown JSON field must not change Confirmed state");
        }
        relay.sendJsonState(R"({"power":true})");
        check(eventually([&] {
            return confirmed(executor, true, "device_g1_json_probe");
        }), "valid complete JSON state should become Confirmed");

        testGenerationRetirement(proposed, relay);

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

        // Ask the external loopback-only fixture to stop/restart its broker.
        // Verify that the live subscriber invalidates stale evidence and
        // recovers using a NEW state message, without replaying a command.
        const std::string controlDir(argv[3]);
        { std::ofstream request(controlDir + "/drop-request");
          check(static_cast<bool>(request << "drop\n"), "failed to request broker restart"); }
        check(eventually([&] {
            const auto state = executor.production().device("device_g1_relay");
            return state && state->connectivity == execution::Connectivity::Unreachable &&
                   state->properties.at("power").knowledge == execution::StateKnowledge::Unknown;
        }, 7000), "subscriber disconnect did not invalidate stale Confirmed state");

        check(eventually([&] {
            std::ifstream marker(controlDir + "/broker-restarted");
            return marker.good();
        }, 7000), "isolated broker fixture did not restart");
        check(eventually([&] {
            std::lock_guard<std::mutex> lock(relay.mutex);
            return relay.subscribed;
        }, 10000), "simulated relay did not re-subscribe after broker restart");

        // The device, not the command executor, supplies a fresh state sample.
        int restoreProbes = 0;
        check(eventually([&] {
            if ((restoreProbes++ % 10) == 0) relay.sendState("OFF", false);
            return confirmed(executor, false);
        }, 8000), "Firefly subscriber did not recover with new state telemetry");
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        check(relay.commandCount() == 2, "command replayed when broker reconnected");
        std::cout << "PASS isolated MQTT 3.1.1 broker ON QoS1 / OFF QoS0, "
                  << "retained ignored, subscribe reconnect, no command replay\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "MQTT integration FAIL: " << e.what() << '\n';
        return 1;
    }
}
