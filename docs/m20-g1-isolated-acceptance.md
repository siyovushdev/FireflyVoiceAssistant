# M20 G1 — isolated MQTT broker acceptance

Parent: #29. Tasks: #31 (external publisher) and #32 (subscription/state). G0 #30 is independently accepted. **This file is a test recipe, not a PASS claim.**

## Safety boundary

Run exclusively on a Linux development host in an isolated directory. Never restart or reconfigure the production Firefly, its systemd services, NPU, voice runtime, live ConfigStore or an existing broker. The Python fixture starts a fresh local Mosquitto process on `127.0.0.1`, an ephemeral port and an isolated temporary configuration, with anonymous access **only inside that loopback-only fixture**. It does not enable the Firefly system broker.

## Prerequisites

- Real `libmosquitto` development headers and shared library, and a standalone `mosquitto` binary, all pre-provisioned in the **test** environment. No source-level fake library may count as MQTT=ON acceptance.
- A working C++17 compiler and CMake, Python 3, build dependencies already required by the Firefly host CPU tests.
- For the separate frontend checks: existing reproducible Node dependencies.

If `libmosquitto` or Mosquitto is absent, report the real test as `NOT_RUN`. Do not silently substitute MQTT=OFF or stub implementations.

## Commands

```bash
cmake -S . -B /tmp/firefly-m20-g1-build \
  -DFIREFLY_HOST_OFFLINE_TESTS=ON \
  -DFIREFLY_BUILD_ASSISTANT=OFF \
  -DFIREFLY_BUILD_VOICE=OFF \
  -DFIREFLY_BUILD_CONFIG_TOOLS=ON \
  -DFIREFLY_ENABLE_MQTT=ON \
  -DBUILD_TESTING=ON
cmake --build /tmp/firefly-m20-g1-build --parallel 4
ctest --test-dir /tmp/firefly-m20-g1-build --output-on-failure -R 'MqttBrokerIntegration|MqttInitialBrokerRecovery|MqttObservationSafety'
ctest --test-dir /tmp/firefly-m20-g1-build --output-on-failure
```

CMake builds the native broker test only with the real library. The `MqttBrokerIntegration` and `MqttInitialBrokerRecovery` CTests are registered only if `mosquitto` is found. Confirm **both** appear in `ctest -N` and actually run before claiming MQTT G1 broker PASS.

## Covered by the new loopback broker smoke

- Explicit preview causes zero MQTT hardware publishes.
- Retained old state never becomes Confirmed merely from subscription.
- Fresh separate state topic confirms real ON/OFF as delivered by a simulated physical relay.
- Literal command topic/payload, QoS1 ON and QoS0 OFF.
- Wrong-target reject and no extra publishes in the ordinary successful sequence.
- Broker disconnect revokes stale Confirmed/Reachable, a subscriber reconnects, fresh telemetry restores Confirmed, and an already completed command is not replayed.
- Separate cold-start fixture keeps the broker **absent** until the subscription actually reaches Unreachable, then starts it without issuing another Apply. Bounded subscription-only retry must reconnect, accept new non-retained telemetry and never replay a hardware command.
- Anonymous loopback test connections omit the optional `credentials_id` entirely; do not weaken ConfigStore validation to accept an empty stable ID.
- Real `RuntimeGenerations::apply` remaps an MQTT state topic while callers intentionally retain old generation references: the old subscription must stop immediately after successful Apply. Removal of an observation must also retire the prior subscription; rollback creates a new generation that only accepts fresh telemetry from the restored topic.
- `RuntimeGenerations::preflight` and failed Apply must *not* retire the currently active subscriber. These cases run in the same broker integration smoke through a lightweight CPU-only `ContextModel` seam and actual MQTT-enabled generation/executor classes.
- The test broker and its configuration are destroyed after completion.

## Resource limits

The candidate bounds simultaneously executing one-shot publish clients to 16 per process and allows at most 32 subscription clients per active runtime generation, with at most 128 topics per subscription client. Overflow must fail closed (never launch extra network threads or fabricate state). These limits are implementation policy in G1 and should be independently tested, including concurrent busy returns and clean lifecycle shutdown.

## Still required for full G1 acceptance

The loopback smoke exercises runtime generation remap/removal/rollback, but it does **not** prove full Web Admin Save→CAS→Apply/recovery with persisted ConfigStore, HTTP server and all owner acceptance paths. It also does **not** replace independent review and controlled failure injection: TLS CA/hostname validation, auth/credential references, different external brokers, QoS1 unacknowledged network loss/no replay, subscribe refusal/ACL, malformed state fields and duplicate JSON, multi-target concurrency and bound resources, delayed SUBACK/PUBACK, transition across Apply/recovery, and release packaging. Record each as PASS/FAIL/NOT_RUN with evidence. #36 will extend this into complete isolated E2E; #38 is physical hardware acceptance.

## Review gates

- Run original #30 adversarial regressions and independently check the new publisher/subscription callback lifecycle, lock ordering and error states. Browser, hardware, TSAN/ASAN are not implied by a successful CPU test.
- Report exact SHA, dependency and broker versions, literal build/test commands and their outputs, publish counts, subscription/reconnect traces and negative cases.
- Keep #31/#32 OPEN until their explicit acceptance criteria have independently passed; do not infer M20 completion from this recipe.
