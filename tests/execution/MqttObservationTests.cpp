#include "firefly/execution/MqttHardwareExecutor.h"
#include "firefly/execution/DeviceStateStore.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/actions/ScalarJson.h"
#include <iostream>
#include <stdexcept>

using namespace firefly;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F f) {
    bool rejected = false;
    try { f(); } catch (const std::exception&) { rejected = true; }
    check(rejected, "expected rejected MQTT state sample");
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        config::ObservationBinding observation;
        observation.executor = config::ExecutorKind::Mqtt;
        observation.mqtt.format = "json";
        observation.fields = {{"power", "power"}};
        const auto good = execution::decodeMqttObservationPayload(observation, R"({"power":true})");
        check(good.at("power") == actions::Scalar(true), "JSON state scalar parsed");
        rejects([&] { (void)execution::decodeMqttObservationPayload(observation, R"({"power":false,"power":true})"); });
        rejects([&] { (void)execution::decodeMqttObservationPayload(observation, R"({"power":{} })"); });
        rejects([&] { (void)execution::decodeMqttObservationPayload(observation, R"({"power":true,"nested":{"a":1,"a":2}})"); });
        observation.mqtt.format = "text";
        observation.mqtt.field = "power";
        observation.mqtt.truePayload = "ON";
        observation.mqtt.falsePayload = "OFF";
        check(execution::decodeMqttObservationPayload(observation, "OFF").at("power") == actions::Scalar(false),
              "TEXT boolean state parsed");
        rejects([&] { (void)execution::decodeMqttObservationPayload(observation, "unknown"); });

        // Reproduce pre-command/while-connecting observations at exactly the
        // same millisecond. Only samples starting after the barrier may confirm.
        auto base = config::ConfigLoader::fromFile(std::string(argv[1]) + "/config/current-m4.json");
        std::int64_t instant = 1000;
        execution::DeviceStateStore state(base, [&] { return instant; }, [&] { return instant; });
        execution::MqttObservationGate gate;
        const std::string target = "device_kitchen_light";
        const auto staleTicket = gate.snapshot();
        gate.begin(target);
        const auto duringConnect = gate.snapshot();
        check(!gate.accept(target, duringConnect, [&] {
            state.applyObservation({target, {{"brightness", actions::Scalar(std::int64_t{0})}}, 0, {}});
        }), "pre-publish observation cannot confirm while command is in-flight");
        state.applyPrediction(target, {{"brightness", actions::Scalar(std::int64_t{80})}}, instant);
        gate.end(target);
        check(!gate.accept(target, staleTicket, [&] {
            state.applyObservation({target, {{"brightness", actions::Scalar(std::int64_t{0})}}, 0, {}});
        }), "delayed pre-command sample must not confirm after command");
        check(!gate.accept(target, duringConnect, [&] {
            state.applyObservation({target, {{"brightness", actions::Scalar(std::int64_t{0})}}, 0, {}});
        }), "delayed in-flight sample must not confirm after command");
        check(state.device(target)->properties.at("brightness").knowledge == execution::StateKnowledge::Predicted,
              "prediction remains pending until a new broker state");
        const auto freshTicket = gate.snapshot();
        check(gate.accept(target, freshTicket, [&] {
            state.applyObservation({target, {{"brightness", actions::Scalar(std::int64_t{80})}}, 0, {}});
        }), "fresh message accepted at same millisecond");
        check(state.device(target)->properties.at("brightness").knowledge == execution::StateKnowledge::Confirmed,
              "new MQTT state becomes Confirmed even at same millisecond");
        std::cout << "PASS MQTT codec duplicates and command/observation barrier\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
