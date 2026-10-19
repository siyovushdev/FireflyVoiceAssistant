#include "firefly/execution/MqttHardwareExecutor.h"
#include "firefly/execution/DeviceStateStore.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/actions/ScalarJson.h"
#include "../../src/execution/MqttProcess.h"
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
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
        // Every top-level JSON member must be explicitly mapped. Otherwise
        // a malicious/incorrect payload can be partially applied as Confirmed.
        rejects([&] { (void)execution::decodeMqttObservationPayload(observation, R"({"power":true,"unmapped":7})"); });
        rejects([&] { (void)execution::decodeMqttObservationPayload(observation, R"({"power":true,"unmapped":{"nested":false}})"); });
        rejects([&] { (void)execution::decodeMqttObservationPayload(observation, R"({"power":true,"unmapped":[1,2]})"); });
        rejects([&] { (void)execution::decodeMqttObservationPayload(observation, R"({"power":true,"unmapped":null})"); });
        rejects([&] { (void)execution::decodeMqttObservationPayload(observation, R"({"power":true,"unmapped":"text"})"); });
        rejects([&] { (void)execution::decodeMqttObservationPayload(observation, R"({"unexpected":true})"); });
        // Still accept fully declared multi-property JSON without coercion.
        observation.fields = {{"power","power"},{"brightness","brightness"}};
        const auto both = execution::decodeMqttObservationPayload(
            observation, R"({"power":true,"brightness":55})");
        check(both.size() == 2 &&
              both.at("power") == actions::Scalar(true) &&
              both.at("brightness") == actions::Scalar(std::int64_t{55}),
              "fully mapped two-property JSON is accepted");
        rejects([&] { (void)execution::decodeMqttObservationPayload(
            observation, R"({"power":true,"unknown":55})"); });
        observation.fields = {{"power","power"}};
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
        // Check the exact Decode -> DeviceStateStore boundary on real classes:
        // malformed or extra JSON must not partially mutate Unknown/Confirmed.
        config::ObservationBinding strictBrightness;
        strictBrightness.executor = config::ExecutorKind::Mqtt;
        strictBrightness.targetId = target;
        strictBrightness.mqtt.format = "json";
        strictBrightness.fields = {{"brightness", "brightness"}};
        execution::DeviceStateStore strictStore(base, [&] { return instant; }, [&] { return instant; });
        const auto assertStrictState = [&](execution::StateKnowledge expectedKnowledge,
                                           std::optional<actions::Scalar> expectedValue) {
            const auto snapshot = strictStore.device(target);
            check(snapshot.has_value(), "strict JSON target exists");
            const auto& property = snapshot->properties.at("brightness");
            check(property.knowledge == expectedKnowledge && property.value == expectedValue,
                  "strict JSON reject must not mutate state");
        };
        assertStrictState(execution::StateKnowledge::Unknown, std::nullopt);
        for (const std::string& payload : {
                 R"({"brightness":55,"unmapped":7})",
                 R"({"brightness":55,"unmapped":{"nested":false}})",
                 R"({"brightness":55,"unmapped":[1,2]})",
                 R"({"brightness":55,"unmapped":null})"
             }) {
            rejects([&] {
                const auto props = execution::decodeMqttObservationPayload(strictBrightness, payload);
                strictStore.applyObservation({target, props, 0, std::nullopt});
            });
            assertStrictState(execution::StateKnowledge::Unknown, std::nullopt);
        }
        const auto strictValid = execution::decodeMqttObservationPayload(
            strictBrightness, R"({"brightness":55})");
        strictStore.applyObservation({target, strictValid, 0, std::nullopt});
        assertStrictState(execution::StateKnowledge::Confirmed, actions::Scalar(std::int64_t{55}));
        rejects([&] {
            const auto props = execution::decodeMqttObservationPayload(
                strictBrightness, R"({"brightness":80,"unmapped":false})");
            strictStore.applyObservation({target, props, 0, std::nullopt});
        });
        assertStrictState(execution::StateKnowledge::Confirmed, actions::Scalar(std::int64_t{55}));
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
        // The mismatch is itself a valid observation: the matching ON must
        // carry a later arrival ticket to be accepted.
        const auto matchingEarlyTicket = gate.snapshot(target);
        check(gate.accept(target, matchingEarlyTicket, target80, state),
              "newer early confirmation is buffered before PUBACK");
        check(!gate.accept(target, matchingEarlyTicket, old0, state),
              "duplicate ticket must not revoke buffered confirmation");
        // These malformed samples carry fresh tickets but cannot advance the
        // valid-observation watermark or evict the existing buffered value.
        const auto malformedTextTicket = gate.snapshot(target);
        rejects([&] {
            (void)gate.accept(target, malformedTextTicket,
                {{"brightness", actions::Scalar(std::string("invalid"))}}, state);
        });
        const auto malformedBoolTicket = gate.snapshot(target);
        rejects([&] {
            (void)gate.accept(target, malformedBoolTicket,
                {{"brightness", actions::Scalar(true)}}, state);
        });
        const auto lateCallbackTicket = gate.snapshot(target);
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
        check(gate.accept(target, lateCallbackTicket, target80, state),
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
        // Simulate command result reporting reachable AFTER losing subscribe:
        // final disconnect status must still win.
        const auto disconnectedTicket = gate.snapshot(disconnectTarget);
        state.markReachable(disconnectTarget, instant);
        state.applyPrediction(disconnectTarget, target80, instant);
        gate.end(disconnectTarget, state, true);
        check(!gate.accept(disconnectTarget, disconnectedTicket, target80, state),
              "callback captured during disconnect cannot confirm after command");
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
        std::uint64_t failedInFlightTicket = 0;
        check(gate.submitPublication(failedTarget, [&] {
                  failedInFlightTicket = gate.snapshot(failedTarget);
                  return 4;
              }) == 4, "library submission failure propagated");
        // Snapshot tickets are monotonic even when submission fails;
        // the publish cutoff must remain closed instead.
        check(!gate.accept(failedTarget, beforeFailedSubmit, target80, state),
              "failed submission cannot buffer pre-submit state");
        check(!gate.accept(failedTarget, failedInFlightTicket, target80, state),
              "failed submission cannot buffer an otherwise eligible in-flight ON");
        gate.end(failedTarget, state, false);
        check(state.device(failedTarget)->properties.at("brightness").knowledge ==
                  execution::StateKnowledge::Unknown, "failed command has no fabricated confirmation");
        // A callback captured before the API call must stay obsolete even
        // when accept() is delayed by the gate mutex. A genuine response
        // captured DURING publish() must survive the return-time boundary.
        // Use fresh state so a previous Confirmed cannot mask this regression.
        const std::string raced = "device_kitchen_light";
        execution::DeviceStateStore raceState(base, [&] { return instant; }, [&] { return instant; });
        execution::MqttObservationGate raceGate;
        raceGate.begin(raced, target80);
        const auto oldBeforePublish = raceGate.snapshot(raced);
        std::mutex syncMutex;
        std::condition_variable syncCv;
        bool enteredPublish = false, leavePublish = false, callbackEntered = false;
        bool earlyAccepted = false;
        int publicationCode = -1;
        std::thread publisher([&] {
            publicationCode = raceGate.submitPublication(raced, [&] {
                std::unique_lock<std::mutex> lk(syncMutex);
                enteredPublish = true;
                syncCv.notify_all();
                syncCv.wait(lk, [&] { return leavePublish; });
                return 0;
            });
        });
        {
            std::unique_lock<std::mutex> lk(syncMutex);
            syncCv.wait(lk, [&] { return enteredPublish; });
        }
        const auto earlyOnTicket = raceGate.snapshot(raced);
        std::thread callback([&] {
            {
                std::lock_guard<std::mutex> lk(syncMutex);
                callbackEntered = true;
            }
            syncCv.notify_all();
            earlyAccepted = raceGate.accept(raced, earlyOnTicket, target80, raceState);
        });
        {
            std::unique_lock<std::mutex> lk(syncMutex);
            syncCv.wait(lk, [&] { return callbackEntered; });
            leavePublish = true;
        }
        syncCv.notify_all();
        publisher.join();
        callback.join();
        check(publicationCode == 0, "publication succeeded");
        check(!raceGate.accept(raced, oldBeforePublish, target80, raceState),
              "old matching ON cannot borrow a new ticket after waiting on mutex");
        check(!raceGate.accept(raced, oldBeforePublish, old0, raceState),
              "old OFF cannot invalidate a fresh in-flight reply");
        check(earlyAccepted, "ON delivered during publish return is buffered");
        check(raceState.device(raced)->properties.at("brightness").knowledge ==
                  execution::StateKnowledge::Unknown,
              "early callback must not confirm before prediction");
        raceState.applyPrediction(raced, target80, instant);
        raceGate.end(raced, raceState, true);
        const auto earlyConfirmed = raceState.device(raced)->properties.at("brightness");
        check(earlyConfirmed.knowledge == execution::StateKnowledge::Confirmed &&
                  earlyConfirmed.value == actions::Scalar(std::int64_t{80}),
              "early ON before publish returned must become Confirmed");

        // A later, valid contradictory OFF still cancels buffered ON.
        // Validate separately so it cannot hide the early-positive scenario.
        raceGate.begin(raced, target80);
        check(raceGate.submitPublication(raced, [] { return 0; }) == 0,
              "second publication submitted");
        const auto freshOn = raceGate.snapshot(raced);
        check(raceGate.accept(raced, freshOn, target80, raceState),
              "second command has valid buffered ON");
        const auto freshOff = raceGate.snapshot(raced);
        check(!raceGate.accept(raced, freshOff, old0, raceState),
              "new contradictory OFF revokes buffered ON");
        raceState.applyPrediction(raced, target80, instant);
        raceGate.end(raced, raceState, true);
        check(raceState.device(raced)->properties.at("brightness").knowledge ==
                  execution::StateKnowledge::Predicted,
              "contradictory OFF must prevent stale Confirmed ON");

        // R6: callback arrival order and callback processing order may differ.
        // A newer, well-typed OFF must fence out an older ON, including when
        // both callbacks carry the same wall/steady clock millisecond.
        execution::DeviceStateStore reversedState(base, [&] { return instant; }, [&] { return instant; });
        execution::MqttObservationGate reversedGate;
        reversedGate.begin(target, target80);
        check(reversedGate.submitPublication(target, [] { return 0; }) == 0,
              "reverse-order publication succeeded");
        const auto olderOnTicket = reversedGate.snapshot(target);
        const auto newerOffTicket = reversedGate.snapshot(target);
        check(newerOffTicket > olderOnTicket, "tickets reflect arrival order");
        check(!reversedGate.accept(target, newerOffTicket, old0, reversedState),
              "newer OFF is valid but contradicts requested ON");
        check(!reversedGate.accept(target, olderOnTicket, target80, reversedState),
              "older ON cannot rebuild the cleared early buffer");
        reversedState.applyPrediction(target, target80, instant);
        reversedGate.end(target, reversedState, true);
        check(reversedState.device(target)->properties.at("brightness").knowledge ==
                  execution::StateKnowledge::Predicted,
              "reordered callbacks must not fabricate Confirmed ON");

        // Even after end(), the same reverse processing must not allow a
        // late older ON to overwrite a newer Confirmed OFF.
        execution::DeviceStateStore lateState(base, [&] { return instant; }, [&] { return instant; });
        execution::MqttObservationGate lateGate;
        lateGate.begin(target, target80);
        check(lateGate.submitPublication(target, [] { return 0; }) == 0,
              "late observation publication succeeded");
        const auto lateOldOn = lateGate.snapshot(target);
        const auto lateNewOff = lateGate.snapshot(target);
        lateState.applyPrediction(target, target80, instant);
        lateGate.end(target, lateState, true);
        check(lateGate.accept(target, lateNewOff, old0, lateState),
              "latest OFF observed after command becomes Confirmed");
        check(!lateGate.accept(target, lateOldOn, target80, lateState),
              "older ON after end cannot overwrite later Confirmed OFF");
        const auto lateFinal = lateState.device(target)->properties.at("brightness");
        check(lateFinal.knowledge == execution::StateKnowledge::Confirmed &&
                  lateFinal.value == actions::Scalar(std::int64_t{0}),
              "latest OFF stays Confirmed with same-ms reordered callbacks");

        // Invalid newer payload does not raise the valid watermark. A valid
        // older callback must remain processable afterwards.
        execution::DeviceStateStore invalidState(base, [&] { return instant; }, [&] { return instant; });
        execution::MqttObservationGate invalidGate;
        invalidGate.begin(target, target80);
        check(invalidGate.submitPublication(target, [] { return 0; }) == 0,
              "invalid-order publication succeeded");
        const auto validOlderOn = invalidGate.snapshot(target);
        const auto malformedNewer = invalidGate.snapshot(target);
        rejects([&] { (void)invalidGate.accept(target, malformedNewer,
            {{"brightness", actions::Scalar(std::string("wrong"))}}, invalidState); });
        check(invalidGate.accept(target, validOlderOn, target80, invalidState),
              "invalid newer sample does not fence valid earlier ON");
        invalidState.applyPrediction(target, target80, instant);
        invalidGate.end(target, invalidState, true);
        check(invalidState.device(target)->properties.at("brightness").knowledge ==
                  execution::StateKnowledge::Confirmed,
              "invalid newer state did not evict valid confirmation");

        // Reversed processing of older OFF and newer matching ON must retain
        // the newest ON. A validated contradiction cannot overwrite newer ON.
        execution::DeviceStateStore newestState(base, [&] { return instant; }, [&] { return instant; });
        execution::MqttObservationGate newestGate;
        newestGate.begin(target, target80);
        check(newestGate.submitPublication(target, [] { return 0; }) == 0,
              "newest-ON publication succeeded");
        const auto olderOff = newestGate.snapshot(target);
        const auto newerOn = newestGate.snapshot(target);
        check(newestGate.accept(target, newerOn, target80, newestState),
              "newest matching ON is buffered");
        check(!newestGate.accept(target, olderOff, old0, newestState),
              "older OFF cannot revoke newer matching ON");
        newestState.applyPrediction(target, target80, instant);
        newestGate.end(target, newestState, true);
        check(newestState.device(target)->properties.at("brightness").knowledge ==
                  execution::StateKnowledge::Confirmed,
              "newest ON remains Confirmed");
        // A second command for the same MQTT target is refused without
        // replacing the first command's expected state or opening a new phase.
        execution::MqttObservationGate busyGate;
        busyGate.begin(target, target80);
        const auto originalBusyTicket = busyGate.snapshot(target);
        rejects([&] { busyGate.begin(target, old0); });
        check(busyGate.submitPublication(target, [] { return 0; }) == 0,
              "busy rejection does not damage original publication phase");
        const auto latestBusyTicket = busyGate.snapshot(target);
        check(latestBusyTicket > originalBusyTicket, "busy publication opened one new phase");
        check(busyGate.accept(target, latestBusyTicket, target80, state),
              "original ON command still owns its MQTT observation window");
        busyGate.end(target, state, false);

        // A tentative IPC window can buffer samples, but a sample captured
        // before the worker's real API barrier cannot later establish Confirmed.
        execution::DeviceStateStore ipcState(base, [&] { return instant; }, [&] { return instant; });
        execution::MqttObservationGate ipcGate;
        execution::detail::MqttSharedOrder workerOrder(ipcGate.publicationOrderDescriptor());
        check(workerOrder.valid(), "worker shares sealed ordering counter");
        ipcGate.begin(target, target80);
        const auto permit = ipcGate.authorizePublication(target);
        const auto inIpcGap = ipcGate.snapshot(target);
        check(ipcGate.accept(target, inIpcGap, target80, ipcState), "tentative sample buffered only");
        const auto atSubmit = workerOrder.next();
        check(atSubmit > permit, "actual worker barrier follows permit");
        ipcGate.publicationSubmitted(target, atSubmit);
        ipcState.applyPrediction(target, target80, instant);
        ipcGate.end(target, ipcState, true);
        check(ipcState.device(target)->properties.at("brightness").knowledge == execution::StateKnowledge::Predicted,
              "pre-submit sample from IPC gap never confirms");
        ipcGate.begin(target, target80);
        ipcGate.authorizePublication(target);
        const auto actual = workerOrder.next();
        const auto earlyReply = ipcGate.snapshot(target);
        check(ipcGate.accept(target, earlyReply, target80, ipcState), "early real reply buffered before IPC ack");
        ipcGate.publicationSubmitted(target, actual);
        ipcState.applyPrediction(target, target80, instant);
        ipcGate.end(target, ipcState, true);
        check(ipcState.device(target)->properties.at("brightness").knowledge == execution::StateKnowledge::Confirmed,
              "actual early reply survives IPC scheduling and prediction");
        ipcGate.begin(target, target80);
        ipcGate.authorizePublication(target);
        ipcGate.accept(target, ipcGate.snapshot(target), target80, ipcState);
        ipcState.applyPrediction(target, target80, instant);
        ipcGate.end(target, ipcState, true);
        check(ipcState.device(target)->properties.at("brightness").knowledge == execution::StateKnowledge::Predicted,
              "missing actual submission is fail closed even if accepted is supplied");

        std::cout << "PASS MQTT codec duplicates and command/observation barrier\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
