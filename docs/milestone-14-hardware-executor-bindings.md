# Milestone 14 — HardwareExecutor + Bindings foundation

Resume baseline: `5a17dda` (Milestone 13). This milestone adds the first
production-capable physical execution path without changing the semantic stack
(Qwen3-4B / RKLLM context 1024, SafetyGate, GenericAction, STT, Admin Copilot).

## Semantic vs physical boundary

```text
Natural language
    ↓
CommandEngine (parser / RKLLM)
    ↓
GenericAction          ← semantic candidate only
    ↓
SafetyGate             ← original-input grounding; no transport knowledge
    ↓
Domain validation      ← capability / argument / range checks
    ↓
BindingResolver        ← physical mapping (targetId + actionId → Binding)
    ↓
HardwareExecutor       ← transport-specific (HTTP for M14)
    ↓
physical device
```

LLM prompts, `GenericAction`, Action definitions and SafetyGate **must not**
contain HTTP/MQTT/RS-485/GPIO details. A Binding is only consulted **after**
semantic validation succeeds. A Binding cannot make a denied or out-of-domain
action executable.

## Binding responsibility

Top-level config section `bindings[]` (schema **v2** only; v1 remains empty):

| Field | Role |
| --- | --- |
| `target_id` | Existing device id |
| `action_id` | Action that already belongs to that device |
| `executor` | Transport kind (`http` in M14) |
| `http` | Typed HTTP config (method, url, timeout_ms, body, optional argument) |

ConfigValidator fail-closed checks:

- referenced target/action exist and the action is on that target;
- target+action are enabled and not denied;
- executor is known;
- HTTP URL is `http://` without credentials/fragments;
- timeout is bounded (50–10000 ms);
- optional `argument` names a real action parameter and does not collide with fixed body keys;
- at most one Binding per `(target_id, action_id)`.

## HardwareExecutor responsibility

`ExecutorRegistry` maps `ExecutorKind` → `HardwareExecutor` implementation.
`CommandEngine` / runtime never branch on transport:

```text
if (http) ... else if (mqtt) ...   // forbidden in semantic path
```

M14 registers only `HttpHardwareExecutor`. Adding MQTT later means a new
executor class + registry registration + schema enum value — not a change to
parser, SafetyGate or GenericAction.

`BoundDeviceExecutor` is always installed on `RuntimeGeneration`. Per-request
`execution::ExecutionIntent` chooses the path:

| Intent | Call site | Behavior |
| --- | --- | --- |
| `None` | dry-run / preview | SafetyGate + validate only; no executor |
| `Playground` | Web Admin `/api/test-command` | domain preview → **Mock only**; never BindingResolver / HTTP |
| `Production` | voice / production runtime | domain preview → BindingResolver → fail closed `binding_missing` or HardwareExecutor |

`bindings.empty()` is **not** a mode switch. Diagnostic `source` strings (`admin-test`,
`voice`) are labels for history only.

Shared local mock state inside `BoundDeviceExecutor` still backs observe / reconcile /
playground virtual execute so Admin UI state semantics stay consistent.

## Failure model

| Layer | Examples | Effect |
| --- | --- | --- |
| Semantic | SafetyGate U, `invalid_action_domain` | No Binding lookup, no HTTP |
| Physical | `binding_missing`, `connection_failure`, `timeout`, `http_error` (+ optional detail), `malformed_binding`, `unsupported_executor`, `binding_ambiguous` | No local state mutation |

These layers are intentionally separate diagnostics.

`HardwareResult` is transport-neutral: `{success, error, detail}`. HTTP status belongs
in `detail` (opaque to `BoundDeviceExecutor`), never as a typed HTTP field on the
generic result.

## HTTP URL / DNS

M14 HTTP bindings require a **numeric IPv4** host (`http://192.168.1.50/...`).
DNS hostnames are rejected at validation. `HttpHardwareExecutor` also passes
`AI_NUMERICHOST` to `getaddrinfo`, so resolution cannot block on DNS. Socket
connect/send/read remain bounded by `timeout_ms`. ESP32 PoC uses a LAN IP.

## Retry policy

**No automatic retries** in `HttpHardwareExecutor`. Hardware commands may be
non-idempotent. Retries, if ever added, must be an explicit per-binding or
per-action policy — never a hidden HTTP-client default.

## State semantics

```text
command accepted/executed successfully
    ≠
physical state independently confirmed
```

M14 has no telemetry/readback. After a successful HTTP response (2xx), the
runtime may update **local mock state** so Admin UI stays consistent. That update
means “command accepted”, not “GPIO measured ON”. A future telemetry subsystem
is out of scope.

## How to add another executor later

1. Extend `ExecutorKind` + schema `executor` enum (e.g. `mqtt`).
2. Add a typed config struct (like `HttpBinding`) and validator checks.
3. Implement `MqttHardwareExecutor : HardwareExecutor`.
4. `registry.registerExecutor(ExecutorKind::Mqtt, ...)`.
5. Leave CommandEngine / SafetyGate / GenericAction untouched.

## Sample ESP32 HTTP binding

Conceptual example (schema v2). Adjust host/path to the board firmware:

```json
{
  "target_id": "device_office_lamp",
  "action_id": "action_lamp_on",
  "executor": "http",
  "http": {
    "method": "POST",
    "url": "http://192.168.1.50/relay",
    "timeout_ms": 1500,
    "content_type": "application/json",
    "body": { "power": true }
  }
}
```

Brightness-style argument forwarding:

```json
{
  "target_id": "device_kitchen_light",
  "action_id": "action_set_brightness",
  "executor": "http",
  "http": {
    "method": "POST",
    "url": "http://192.168.1.50/brightness",
    "timeout_ms": 1500,
    "content_type": "application/json",
    "body": {},
    "argument": "brightness"
  }
}
```

See also `config/examples/esp32-http-binding.example.json`.

## Live ESP32 smoke (deferred)

If the ESP32/firmware is absent, M14 is still complete with:

- deterministic fake-HTTP integration tests (`HardwareBindings`);
- documented example binding;
- no requirement to run Qwen/NPU for this milestone.

Live smoke later: one `firefly-demo`, one model process, SSH to the board, apply
a v2 config with the HTTP binding, execute a single Admin test command, confirm
GPIO/LED.
