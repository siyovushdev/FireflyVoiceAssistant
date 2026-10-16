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

        // Early state after MQTT submit but before PUBACK is buffered, not lost.
        // A pre-submit sample (even at the same millisecond) is never admitted.
        auto base = config::ConfigLoader::fromFile(std::string(argv[1]) + "/config/current-m4.json");
        std::int64_t instant = 1000;
        execution::DeviceStateStore state(base, [&] { return instant; }, [&] { return instant; });
        execution::MqttObservationGate gate;
        const std::string target = "device_kitchen_light";
        const std::map<std::string, actions::Scalar> target80{
            {"brightness", actions::Scalar(std::int64_t{80})}};
        const std::map<std::string, actions::Scalar> old0{
            {"brightness", actions::Scalar(std::int64_t{0})}};
        const auto beforeCommand = gate.snapshot(target);
        gate.begin(target, target80);
        const auto whileConnecting = gate.snapshot(target);
        check(!gate.accept(target, whileConnecting, old0, state),
              "an old OFF during broker connection is not a new ON");
        check(!gate.accept(target, whileConnecting, target80, state),
              "even matching state cannot confirm before publishing");

        int submitted = 0;
        check(gate.submitPublication(target, [&] { ++submitted; return 0; }) == 0 &&
                  submitted == 1, "submit must invoke library call exactly once");
        const auto whilePublishing = gate.snapshot(target);
        check(whilePublishing != whileConnecting, "epoch advances only after successful submission");
        check(!gate.accept(target, beforeCommand, old0, state),
              "pre-command ticket rejected");
        check(!gate.accept(target, whileConnecting, old0, state),
              "callback captured before publish cannot enter early buffer");
        check(!gate.accept(target, whilePublishing, old0, state),
              "mismatching OFF cannot confirm requested ON");

        check(gate.accept(target, whilePublishing, target80, state),
              "fresh early confirmation is buffered before PUBACK");
        // This deliberately invalid sample previously displaced valid power:true.
        rejects([&] {
            (void)gate.accept(target, whilePublishing,
                {{"brightness", actions::Scalar(std::string("invalid"))}}, state);
        });
        rejects([&] {
            (void)gate.accept(target, whilePublishing,
                {{"brightness", actions::Scalar(true)}}, state);
        });
        check(state.device(target)->properties.at("brightness").knowledge ==
                  execution::StateKnowledge::Unknown, "buffer must not confirm before predicted state");
        state.applyPrediction(target, target80, instant);
        gate.end(target, state, true);
        const auto confirmed = state.device(target)->properties.at("brightness");
        check(confirmed.knowledge == execution::StateKnowledge::Confirmed,
              "valid buffered reply survives invalid sample and becomes Confirmed");
        check(confirmed.value == actions::Scalar(std::int64_t{80}),
              "badly typed sample did not evict valid early reply");

        // A callback already entered after submit but decoded after PUBACK is valid.
        check(gate.accept(target, whilePublishing, target80, state),
              "late-parsed post-submit callback can confirm");
        // New command revokes the old callback ticket; another device is unaffected.
        const auto beforeSecondCommand = gate.snapshot(target);
        const std::string other = "device_bedroom_light";
        const auto snapshotB = gate.snapshot(other);
        gate.begin(target, target80);
        check(gate.accept(other, snapshotB,
                {{"brightness", actions::Scalar(std::int64_t{35})}}, state),
              "a command on A cannot discard valid telemetry from B");
        check(state.device(other)->properties.at("brightness").knowledge ==
                  execution::StateKnowledge::Confirmed, "unrelated device remains Confirmed");
        check(!gate.accept(target, beforeSecondCommand, old0, state),
              "callbacks from previous command cannot cross another command boundary");
        gate.end(target, state, false);

        // Disconnect invalidates buffered candidates and tickets *before* end().
        const std::string disconnectTarget = "device_living_light";
        const auto livingOld = gate.snapshot(disconnectTarget);
        gate.begin(disconnectTarget, target80);
        check(gate.submitPublication(disconnectTarget, [] { return 0; }) == 0,
              "living command submits");
        const auto livingTicket = gate.snapshot(disconnectTarget);
        check(gate.accept(disconnectTarget, livingTicket, target80, state),
              "living early confirmation buffered");
        gate.disconnected(disconnectTarget, state);
        check(!gate.accept(disconnectTarget, livingTicket, target80, state),
              "disconnect revokes old callback ticket");
        check(state.device(disconnectTarget)->properties.at("brightness").knowledge ==
                  execution::StateKnowledge::Unknown,
              "disconnect clears prior state knowledge");
        // Simulate command return after loss of subscribe connection.
        state.applyPrediction(disconnectTarget, target80, instant);
        gate.end(disconnectTarget, state, true);
        check(state.device(disconnectTarget)->properties.at("brightness").knowledge ==
                  execution::StateKnowledge::Unknown,
              "disconnect revokes both stale buffer and later prediction");
        check(state.device(disconnectTarget)->connectivity == execution::Connectivity::Unreachable,
              "buffered state cannot restore reachability after disconnect");
        check(livingOld != livingTicket, "living command updated ticket");

        // Failed C API submission cannot open the post-submit window at all.
        const std::string failedTarget = "device_corridor_light";
        gate.begin(failedTarget, target80);
        const auto beforeFailedSubmit = gate.snapshot(failedTarget);
        check(gate.submitPublication(failedTarget, [] { return 4; }) == 4,
              "library submission failure propagated");
        check(gate.snapshot(failedTarget) == beforeFailedSubmit,
              "failed submission cannot open post-submit epoch");
        check(!gate.accept(failedTarget, beforeFailedSubmit, target80, state),
              "failed submission cannot buffer state");
        gate.end(failedTarget, state, false);
        check(state.device(failedTarget)->properties.at("brightness").knowledge ==
                  execution::StateKnowledge::Unknown, "failed command has no fabricated confirmation");
        std::cout << "PASS MQTT codec duplicates and command/observation barrier\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
