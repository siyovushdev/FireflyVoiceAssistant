# M20 — MQTT MVP: implementation and verification boundary

Branch: `feature/m20-mqtt-mvp`. This is **development, NOT a production release**.
The original installed M19 release (`m19-c7fa1490-20261015T150156Z`) must remain untouched.

## Implemented in this development branch

- Configuration v2 extends existing Connection/Binding/Observation with literal MQTT
  topic/payload and TEXT boolean or top-level JSON state mappings. HTTP v1/v2
  canonical output is unchanged when MQTT extensions are absent.
- MQTT command publish uses an isolated one-shot libmosquitto client, never an
  automatically reconnecting publisher. QoS 1 remains at-least-once *within the
  attempt*; duplicate delivery cannot be categorically prevented by MQTT.
- Subscription clients are generation-owned. They start **only after Apply**,
  reconnect on failure, ignore retained state at subscription, and invalidate
  knowledge on disconnect. A publish is Accepted, not Confirmed.
- The manual Vue wizard can create shared MQTT Connections and map actions/state.
  HTTP, SafetyGate and AI Setup are not changed.
- Credentials are referenced **only** by identifier and provisioned as owner-only
  files in `/var/lib/firefly/mqtt/credentials/<id>.cred` (first line username,
  second line password, LF-terminated, owner = runtime euid, mode 0600).
  Never commit passwords, model binaries or live ConfigStore.

## Build (developer PC only; no production changes)

With the already available frontend dependencies:

```sh
cd src/admin/frontend
npm run typecheck
npm test
npm run build
cd ../../..
```

Then a fresh explicit Linux-host CPU build:

```sh
cmake -S . -B build/m20-host -DFIREFLY_HOST_OFFLINE_TESTS=ON \
  -DFIREFLY_BUILD_ASSISTANT=OFF -DFIREFLY_BUILD_VOICE=OFF \
  -DFIREFLY_BUILD_ADMIN=ON -DFIREFLY_BUILD_CONFIG_TOOLS=ON \
  -DFIREFLY_ENABLE_MQTT=OFF
cmake --build build/m20-host -j2
ctest --test-dir build/m20-host --output-on-failure
```

The OFF build checks all MQTT configuration and integration against the fail-closed
runtime stub, **not real MQTT delivery**.

For real MQTT code compilation, libmosquitto-dev must already be available on
the test host. Do not install a package on a live Firefly merely to test this:

```sh
cmake -S . -B build/m20-mqtt-host -DFIREFLY_HOST_OFFLINE_TESTS=ON \
  -DFIREFLY_BUILD_ASSISTANT=OFF -DFIREFLY_BUILD_VOICE=OFF \
  -DFIREFLY_BUILD_ADMIN=ON -DFIREFLY_BUILD_CONFIG_TOOLS=ON \
  -DFIREFLY_ENABLE_MQTT=ON
cmake --build build/m20-mqtt-host -j2
ctest --test-dir build/m20-mqtt-host --output-on-failure
```

Capture the original build errors and test totals; do not claim PASS before running.

## Open work before calling M20 complete

1. Local Mosquitto **on/off via Web Admin**, with constrained systemd privilege
   boundary and initial protected broker credentials, is **not implemented**.
2. Provisioning credentials through Web Admin is **not implemented**; a private
   operator-provisioned file is required for authenticated brokers.
3. An end-to-end real Mosquitto fixture and browser/manual acceptance have **not run**.
4. Production packaging must include libmosquitto and verify its dynamic linkage
   without changing existing NPU/audio/ConfigStore/recovery contracts.
5. Actual hardware relay ON/OFF and state topics are unverified.
6. DNS/TLS/reconnect/retained/error testing remains required with a real broker.

No PR, merge, production rollout or installation is authorized by this work.
