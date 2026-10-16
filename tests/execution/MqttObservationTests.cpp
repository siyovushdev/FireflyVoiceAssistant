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

        // Reproduce early device confirmation before PUBACK, including equal
        // millisecond timestamps. Old pre-publish packets still fail closed.
        auto base = config::ConfigLoader::fromFile(std::string(argv[1]) + "/config/current-m4.json");
        std::int64_t instant = 1000;
        execution::DeviceStateStore state(base, [&] { return instant; }, [&] { return instant; });
        execution::MqttObservationGate gate;
        const std::string target = "device_kitchen_light";
        const auto beforeCommand = gate.snapshot(target);
        gate.begin(target);
        const auto whileConnecting = gate.snapshot(target);
        check(!gate.accept(target, whileConnecting,
                           {{"brightness", actions::Scalar(std::int64_t{0})}}, state),
              "pre-publish state cannot confirm a new command");
        gate.publicationStarted(target);
        const auto whilePublishing = gate.snapshot(target);
        check(!gate.accept(target, beforeCommand,
                           {{"brightness", actions::Scalar(std::int64_t{0})}}, state),
              "packet captured before command rejected");
        check(!gate.accept(target, whileConnecting,
                           {{"brightness", actions::Scalar(std::int64_t{0})}}, state),
              "delayed connection-phase state rejected");

        check(gate.accept(target, whilePublishing,
                          {{"brightness", actions::Scalar(std::int64_t{80})}}, state),
              "fresh early device state buffered before broker acknowledgment");
        check(state.device(target)->properties.at("brightness").knowledge == execution::StateKnowledge::Unknown,
              "buffering cannot confirm before prediction");
        state.applyPrediction(target, {{"brightness", actions::Scalar(std::int64_t{80})}}, instant);
        gate.end(target, state);
        check(state.device(target)->properties.at("brightness").knowledge == execution::StateKnowledge::Confirmed,
              "early reply wins over prediction, even in same millisecond");
        check(state.device(target)->properties.at("brightness").value ==
                  actions::Scalar(std::int64_t{80}), "early reply value preserved");
        check(gate.accept(target, whilePublishing,
                          {{"brightness", actions::Scalar(std::int64_t{70})}}, state),
              "late-parsed callback from publish phase still accepted");
        check(state.device(target)->properties.at("brightness").value ==
                  actions::Scalar(std::int64_t{70}), "later message applies after publish returns");

        // A command to kitchen cannot invalidate unrelated bedroom telemetry.
        const std::string other = "device_bedroom_light";
        const auto otherSnapshot = gate.snapshot(other);
        gate.begin(target);
        check(gate.accept(other, otherSnapshot,
                          {{"brightness", actions::Scalar(std::int64_t{35})}}, state),
              "target B observation survives command on A");
        check(state.device(other)->properties.at("brightness").knowledge == execution::StateKnowledge::Confirmed,
              "target B confirmed independently");
        gate.end(target, state);
        check(!gate.accept(target, whilePublishing,
                           {{"brightness", actions::Scalar(std::int64_t{0})}}, state),
              "previous command callback cannot cross a new command boundary");

        // No buffered telemetry is manufactured on failed/aborted commands.
        const auto unknown = "device_living_light";
        gate.begin(unknown);
        gate.end(unknown, state);
        check(state.device(unknown)->properties.at("brightness").knowledge == execution::StateKnowledge::Unknown,
              "failed before publish does not invent state");
        std::cout << "PASS MQTT codec duplicates and command/observation barrier\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
