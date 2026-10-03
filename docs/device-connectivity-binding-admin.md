# Device Connectivity & Binding Admin

## Design / impact report

The production flow already was:

`CommandEngine → SafetyGate → domain validation → BindingResolver → HardwareExecutor → DeviceStateStore`.

M14/M16 stored a full HTTP URL inside each `bindings[]` entry. “Connection” was a
UI label, not a reusable entity. M15 already separated Predicted/Confirmed state,
transport reachability and execution status. ConfigStore and RuntimeGenerations
already supplied immutable catalogs, CAS, backup, LKG, atomic publication and
request serialization. These components are extended, not replaced.

The change adds optional `connections[]` and `observations[]` to configuration v2,
and `connection_id` to the existing Binding. Logical devices, assignments,
LanguageIndex, CommandEngine, SafetyGate, STT and RKLLM settings stay unchanged.
Legacy URL bindings remain readable/executable. Empty extension arrays are omitted
from canonical output, preserving existing store hashes and recovery compatibility.
No store migration, model rebuild or downloaded dependency is required.

## Domain and runtime

```text
Logical Device (room, capabilities, actions, state)
  → Binding (existing target_id + action_id, fixed transport mapping)
  → Connection (stable ID, reusable endpoint)
  → ExecutorRegistry → HttpHardwareExecutor → physical device
  → DeviceStateStore: Predicted

ObservationBinding → same Connection → HTTP GET → validated scalar fields
  → DeviceStateStore: Confirmed
```

Bindings never choose the semantic action. BindingResolver still runs only after
SafetyGate and domain validation. The HTTP executor checks that the resolved
binding target/action matches the submitted GenericAction. It receives a validated
generation, never voice text. Playground stays virtual unless its existing explicit
Production option is selected.

One Connection can serve multiple logical devices. Renames retain its stable ID.
Deleting a referenced connection fails validation. Device removal from the UI or
Copilot removes its command and observation mappings in the same draft. Raw
configuration with dangling references is rejected. Connection IDs participate in
ConfigStore retirement/category checks, just like other stable identities.

A generation change preserves compatible evidence only while its physical mapping
is unchanged. Changing an endpoint or mapping resets that device's physical
knowledge/connectivity to Unknown; renaming a connection retains evidence. Process
restart always starts Unknown. Configuration itself survives restart.

## Configuration example

The `power` boolean state and both actions must already exist. Their assignments
set `power` to true/false; that semantic definition contains no transport details.

```json
{
  "connections": [{
    "id": "connection_controller",
    "name": "ESP32 Test",
    "type": "http",
    "enabled": true,
    "timeout_ms": 2000,
    "http": {"base_url": "http://127.0.0.1:8765", "base_path": ""}
  }],
  "bindings": [
    {
      "target_id": "device_light", "action_id": "action_on",
      "connection_id": "connection_controller", "executor": "http",
      "http": {"method": "POST", "path": "/relay/1", "body": {"on": true}, "expected_status": 200}
    },
    {
      "target_id": "device_light", "action_id": "action_off",
      "connection_id": "connection_controller", "executor": "http",
      "http": {"method": "POST", "path": "/relay/1", "body": {"on": false}, "expected_status": 200}
    }
  ],
  "observations": [{
    "target_id": "device_light", "connection_id": "connection_controller", "executor": "http",
    "http": {"method": "GET", "path": "/relay/1", "expected_status": 200},
    "fields": {"on": "power"}
  }]
}
```

Command mappings support POST, PUT, PATCH, DELETE and body-free GET; GET actions
must be deliberately selected by the administrator. Body is a JSON object of
scalars, retaining the existing typed contract. `argument` copies only a declared
action parameter into the body under that parameter's name. There is no text
substitution, expression evaluation, shell interpolation or voice-supplied URL.

`content_type` defaults to `application/json`. `timeout_ms: 0` (or omission in a
connection mapping) inherits the Connection timeout. An explicit override is
50–10000 ms. `expected_status` is a selected 2xx status; omission retains the legacy
“any 2xx” behavior. Optional headers allow non-secret `Accept` and `X-*` metadata;
framing/Host/authentication headers and control characters are rejected.

## Web Admin

1. Device card → **Настроить подключение** → **По сети** → HTTP / REST.
2. Select an existing Connection or create one: name, IPv4 base URL, optional common
   path, timeout and enabled flag. The endpoint is shared by all referring devices.
3. **Проверить соединение** opens and closes a TCP socket only. It sends **zero HTTP
   bytes**, does not invoke a device action, and does not update DeviceStateStore.
   A successful probe proves TCP reachability, not API correctness or TLS support.
4. Configure each existing action's method, path, JSON body and expected status.
   Body is available in Simple mode. Optional headers/timeouts are under details.
5. Configure Observe separately: GET path and response member → state field.
6. **Preview** validates the full draft and shows the diff. **Apply** requires the
   exact previewed draft, performs the existing CAS save/apply and publishes the
   generation. No hardware request occurs during validation/save/apply.
7. **Cancel** restores the draft snapshot, including setup-session draft state.
   **Оставить в черновике** defers publication to the existing whole-home flow.
   During a setup session the existing setup Preview/Apply/Cancel owns publication.

After Apply, **Прочитать состояние** on a production state card performs one GET
using the active mapping. It requires the expected active hash to prevent a stale
UI from polling a changed generation. No implicit polling, heartbeat or automatic
command retry exists.

All new HTTP Admin endpoints retain the same-origin mutation guard and
`X-Firefly-Admin: 1` requirement:

- `POST /api/connections/test`: `{configuration, connection_id}`; validated draft,
  connect-only probe, no persistence or physical command.
- `POST /api/devices/poll`: `{target_id, expected_hash}`; active mapping only,
  serialized with command execution and generation publication.

The existing `/api/devices/observe` remains the explicitly simulated/external
observation API; the new poll path does not accept caller-provided state values.

## Observe and errors

One observation mapping per device; multiple top-level JSON fields may map to
existing distinct state properties. Values must already have the correct scalar
type and satisfy the state schema. No nested JSONPath, coercion or transformation.
All mapped values are validated before any state property changes.

Accepted HTTP command → Predicted. Only an independently successful GET and schema
validation → Confirmed. Malformed JSON, duplicate keys, missing members, wrong
types, bad statuses, oversized responses and timeouts leave property evidence
unchanged. Reachability is tracked independently, as before.

HTTP reads complete Content-Length, basic chunked or connection-close framing.
Fragmented packets are supported. Conflicting framing, truncated responses,
compressed content, chunk extensions and trailers fail closed. Redirects are
never followed; a 3xx response is an execution failure.

## Network decisions and limitations

- Only numeric IPv4 `http://` is implemented. HTTPS and DNS fail validation with an
  explicit explanation. The target has no TLS/HTTP development dependencies;
  none were downloaded. A later reviewed HTTPS client can extend this transport.
- Loopback, private LAN and public numeric IPv4 are permitted deliberately. This is
  locally administered equipment configuration; a private-only allowlist would
  silently exclude legitimate integrations. There is no DNS rebinding surface.
- URL credentials, fragments, non-HTTP schemes, authority-changing paths,
  control characters and URL templates are rejected. Redirects and proxy settings
  are never interpreted. Socket I/O uses one monotonic deadline, no DNS call.
- Zero automatic retries, including for timeouts and partial command responses.
  A timed-out POST may have reached the device: its state is not asserted; use an
  explicit observation before deciding whether to issue another command.
- Request body limit 16 KiB; response body 64 KiB; response headers 8 KiB; JSON
  observation depth 16. No raw response body or credential logging.
- No authentication/credentials store was added. Do not put tokens in the visible
  body, URL or header fields. Existing config files are not a secrets vault.

MQTT/TCP/UDP have planned `{network:{host,port}}` schemas; Serial has
`{serial:{port,baud_rate}}` (UART, USB Serial, RS-485); CAN has
`{can:{interface}}`. All require `enabled:false`; no executor is registered and an
HTTP mapping cannot reference them. USB is not a transport enum. UI presents
planned choices as disabled; no autodiscovery or physical device firmware exists.

## Copilot

`set_http_connection`, `set_connection_binding`, `set_http_observe` are structured
proposal operations. The existing strict decoder validates the full resulting
catalog and exact operation fields; current device/action/state IDs are supplied
as context. Connection proposals cannot also modify semantic entities or
capabilities. Missing actions and foreign-device actions fail validation. Stale
proposal acceptance checks the draft hash. Neither proposal nor acceptance
performs network I/O, saves or applies configuration. The older explicit URL
binding operation remains compatible.

Transport configuration does not enter the domestic command prompt. Large Admin
inventories remain subject to the existing model/context admission limits. New
proposal operations are regression-tested with structured model outputs; this
milestone does not claim fresh live-NPU natural-language Copilot accuracy.

## Verification

Local reusable equipment mock:

```sh
python3 scripts/mock-http-device.py --port 8765
```

It implements POST/GET `/relay/1` with boolean `on`. It has no connection to real
hardware. The older V1 brightness mock remains available for previous scenarios.

```sh
cmake -S . -B build -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

New suites: `ConnectivityConfig`, `ConnectivityHttp`, `ConnectivityWeb`.
The HTTP integration runs the actual unified production runtime with only the
RKLLM vendor API stubbed. The commands must take `INDEX_EXACT`, pass SafetyGate,
and hit the real localhost socket executor. The browser suite uses real Chromium
and the existing test model seam for home creation, then configures HTTP through
visible controls. It does not require real equipment or run inference on the NPU.

Verified on the Linux/aarch64 workspace on 2026-10-02: full build completed;
**CTest 65/65 passed, 0 failed, 129.85 seconds**. ConnectivityConfig passed in
0.40 s; the complete run includes both localhost HTTP acceptance and Chromium UI
acceptance (14.54 s). `git diff --check` also passed. No commit or push was made.

Actual acceptance covers:

- Empty home creation, existing logical ON/OFF actions, Connection + mappings,
  Preview, Apply and runtime generation publication; no request before Apply.
- Exactly one correct POST for each ON/OFF, then independent GET and
  Predicted → Confirmed; contradictory observation takes precedence.
- Process restart, persisted mappings, initially Unknown state and repeated ON/OFF.
- Shared connection for multiple devices, stable-ID rename, reference-safe removal,
  backup/LKG recovery of the connectivity graph and stale CAS/proposal/poll rejection.
- Binding missing, disabled/unavailable connection, timeout, 400/500/unexpected
  success status, redirect rejection, body/header response limits, truncated and
  fragmented/chunked responses, malformed/duplicate/wrong-type observation JSON.
- Unsupported/ambiguous/unsafe commands, no HTTP on semantic rejection, invalid
  mappings, URL schemes/credentials/header injection/templates and missing Admin guard.
- Chromium forms: connect-only probe, visible ON/OFF JSON bodies, Observe mapping,
  Preview/Cancel/Apply and production state readback, with no JavaScript exceptions.

Next useful work:
reviewed HTTPS/DNS/credential references, then one additional transport using the
same Connection/Binding contracts. Automatic observe scheduling and richer JSON
mapping need their own scope and failure policies.

## Changed files (36)

- [README.md](../README.md)
- [config/schema-v2.json](../config/schema-v2.json)
- [docs/device-connectivity-binding-admin.md](../docs/device-connectivity-binding-admin.md)
- [docs/milestone-16-device-connections-web-admin.md](../docs/milestone-16-device-connections-web-admin.md)
- [include/firefly/admin/RuntimeGeneration.h](../include/firefly/admin/RuntimeGeneration.h)
- [include/firefly/config/Configuration.h](../include/firefly/config/Configuration.h)
- [include/firefly/config/Connectivity.h](../include/firefly/config/Connectivity.h)
- [include/firefly/execution/ActionExecutor.h](../include/firefly/execution/ActionExecutor.h)
- [include/firefly/execution/DeviceStateStore.h](../include/firefly/execution/DeviceStateStore.h)
- [include/firefly/execution/HardwareExecutor.h](../include/firefly/execution/HardwareExecutor.h)
- [include/firefly/execution/HttpHardwareExecutor.h](../include/firefly/execution/HttpHardwareExecutor.h)
- [scripts/mock-http-device.py](../scripts/mock-http-device.py)
- [src/admin/AdminCopilot.cpp](../src/admin/AdminCopilot.cpp)
- [src/admin/AdminDraft.cpp](../src/admin/AdminDraft.cpp)
- [src/admin/Api.cpp](../src/admin/Api.cpp)
- [src/admin/CMakeLists.txt](../src/admin/CMakeLists.txt)
- [src/admin/HttpServer.cpp](../src/admin/HttpServer.cpp)
- [src/admin/RuntimeGeneration.cpp](../src/admin/RuntimeGeneration.cpp)
- [src/admin/RuntimeWorker.cpp](../src/admin/RuntimeWorker.cpp)
- [src/admin/web/admin-model.js](../src/admin/web/admin-model.js)
- [src/admin/web/admin.js](../src/admin/web/admin.js)
- [src/admin/web/connection-editor.js](../src/admin/web/connection-editor.js)
- [src/admin/web/index.html](../src/admin/web/index.html)
- [src/admin/web/simple-editor.js](../src/admin/web/simple-editor.js)
- [src/config/CMakeLists.txt](../src/config/CMakeLists.txt)
- [src/config/ConfigLoader.cpp](../src/config/ConfigLoader.cpp)
- [src/config/ConfigStore.cpp](../src/config/ConfigStore.cpp)
- [src/config/ConfigValidator.cpp](../src/config/ConfigValidator.cpp)
- [src/config/Connectivity.cpp](../src/config/Connectivity.cpp)
- [src/execution/BindingResolver.cpp](../src/execution/BindingResolver.cpp)
- [src/execution/BoundDeviceExecutor.cpp](../src/execution/BoundDeviceExecutor.cpp)
- [src/execution/DeviceStateStore.cpp](../src/execution/DeviceStateStore.cpp)
- [src/execution/HttpHardwareExecutor.cpp](../src/execution/HttpHardwareExecutor.cpp)
- [tests/admin/ConnectivityWebTests.js](../tests/admin/ConnectivityWebTests.js)
- [tests/execution/ConnectivityConfigTests.cpp](../tests/execution/ConnectivityConfigTests.cpp)
- [tests/execution/ConnectivityHttpTests.py](../tests/execution/ConnectivityHttpTests.py)
