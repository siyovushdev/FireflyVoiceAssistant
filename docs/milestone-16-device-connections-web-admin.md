# Milestone 16 — Device Connections in Web Admin

Historical baseline. The current reusable Connection model and HTTP readback are documented in [Device Connectivity & Binding Admin](device-connectivity-binding-admin.md).

Resume baseline: Milestone 15 (`85586d4`). This milestone adds a user-facing
**«Подключение устройства»** editor on top of the existing M14 `bindings[]`
model. Runtime transport architecture is unchanged.

## Concepts

| Term | Role |
| --- | --- |
| **Device / Action** | Semantic product model (what the assistant understands) |
| **Connection** | User-facing hardware configuration in Web Admin |
| **Binding** | Internal mapping `target_id + action_id → executor` in `bindings[]` |
| **Executor** | Runtime implementation (`HttpHardwareExecutor` today) |

```text
Device + Action
→ Binding (config)
→ HardwareExecutor
→ Transport
```

Users see «Подключение»; Advanced mode may show internal ids (`target_id`,
`action_id`, `executor`). Copilot and docs may say `binding` for the same object.

## Simple vs Advanced

### Simple

Per device card:

* connection status (`не настроено` / `HTTP · N из M действий`);
* button **Настроить подключение**;
* editor fields: transport (HTTP), URL, method, argument when the action needs one.

### Advanced

Same draft editor plus the full HTTP binding surface:

* target/action ids;
* executor;
* timeout, content-type;
* fixed body JSON;
* optional GenericAction argument;
* future transports listed as disabled **Planned** (MQTT, RS-485, …).

Both modes edit the same `draft.bindings[]`. No alternate config format.

## Connection completeness (UI only)

Derived from executable actions on the device (enabled device + enabled action,
not denied by `safety.restrictions`):

| Status | Meaning |
| --- | --- |
| `Unconfigured` | 0 bindings for those actions |
| `PartiallyConfigured` | some, but not all |
| `Configured` | every executable action has a binding |

This is **not** a runtime DeviceStateStore axis.

## Draft lifecycle

```text
edit connection in draft
→ Validate  (backend ConfigValidator)
→ Save
→ Apply
```

Editing, Validate, Save, and Apply **never** send HTTP commands to hardware.
Admin Command Playground remains Mock even when real bindings exist (M14 intent
isolation). Production voice/runtime uses bindings only after Apply.

## Test connection

M16 does **not** invent a generic HTTP health-check. Configuration correctness is
validated by the existing Validate path. Physical action tests remain explicit
Admin playground / production commands — never automatic on open/Validate/Save.

## AI Copilot

New operations (proposal only → accept into draft → Validate → Save → Apply):

* `set_http_binding` — requires explicit `method` + `url` (`http://IPv4/…`);
* `clear_http_binding`.

Copilot **must not invent** IP, port, path, protocol, credentials, GPIO, RS-485
addresses, or payload contracts. Incomplete technical data → controlled
`unsupported` / `ambiguous` rejection, empty operations.

## Secrets

HTTP bindings still have no auth fields. UI copy warns against putting passwords
or tokens into visible body fields. A secrets vault is a future productization
concern — out of M16 scope.

## DeviceState (light touch)

Device state cards show production connectivity / knowledge from M15 alongside
the draft connection summary. M16 does not redesign DeviceStateStore.

## Import / export / revisions / diff

`bindings` already participate in configuration diff (`Подключения`), export,
import, revisions, restore, validation, and Apply. Connection edits ride that
path unchanged.

## Intentionally deferred

MQTT, TCP, UDP, RS-485, CAN, GPIO, ESP32 firmware, telemetry transports,
heartbeat, secrets vault, cloud, LLM/STT changes.
