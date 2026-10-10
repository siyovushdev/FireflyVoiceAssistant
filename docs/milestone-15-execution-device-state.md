# Milestone 15 — Execution Lifecycle + Device State foundation

Resume baseline: Milestone 14 (`2873d64`). This milestone separates three
independent axes that must never be collapsed into one enum:

| Axis | Values | Meaning |
| --- | --- | --- |
| **Execution** | `Accepted` \| `Failed` | Did the physical command dispatch succeed? |
| **State knowledge** | `Unknown` \| `Predicted` \| `Confirmed` | How do we know a property value? |
| **Connectivity** | `Unknown` \| `Reachable` \| `Unreachable` | Last transport reachability |
| **Stale** | derived | `knowledge` + process-local steady age + TTL — not a fourth knowledge enum |

## Core principle

```text
successful hardware execution
    ≠
independently confirmed physical state
```

`Accepted` only means the transport accepted the command (e.g. HTTP 2xx).
Local expected state is recorded as **Predicted**. **Confirmed** requires an
independent observation/readback path.

Execution failure and reachability are also independent: HTTP 4xx/5xx is
`Failed` execution with **Reachable** connectivity evidence, because a valid
HTTP response proves the transport endpoint was reached.

## Lifecycle

```text
GenericAction
    ↓
SafetyGate          ← semantic; no DeviceStateStore / connectivity mutation
    ↓
Domain validation
    ↓
BindingResolver
    ↓
HardwareExecutor → HardwareResult {success, error, detail, reachability}
    ↓
Accepted | Failed          ← from success
+ Connectivity update      ← from reachability evidence only
    ↓ (Accepted only)
Predicted property values in DeviceStateStore

independent observation (telemetry / readback / simulated observe API)
    ↓
Confirmed property values (observation wins over Predicted when not stale)
+ Connectivity = Reachable
```

### Reachability evidence (`HardwareResult::reachability`)

Transport-neutral; HTTP status stays in `detail` and is never branched on by
`BoundDeviceExecutor`.

| Evidence | When | Connectivity effect |
| --- | --- | --- |
| `Unreachable` | connection failure / connect or I/O timeout | → Unreachable |
| `Reachable` | valid HTTP response, including 4xx/5xx | → Reachable |
| `None` | malformed binding / unsupported executor / binding missing | unchanged |

Example conflict:

```text
Predicted power = ON
Observation power = OFF
→ Confirmed power = OFF
```

## Classes

### `DeviceStateStore`

Authoritative **production** physical knowledge. Not owned by `HardwareExecutor`.

Per property: `value?`, `knowledge`, `updatedAtMs`, `observedAtMs`. As of
[M18.5](m18-config-state-time-hardening.md), public timestamps remain Unix/wall
time for display; separate non-serialized steady stamps control the unchanged
60-second TTL and local ordering. Wall-clock jumps do not change freshness.

Cold start / process restart → all `Unknown` (M15 has no durable telemetry).
Config-generation Apply may retain in-process Predicted/Confirmed values that
remain schema-compatible. Restart ≠ generation change.

#### Out-of-order observations

Property-level fail-closed ordering now compares `orderSteadyMs` in the local
steady-clock domain. Older local observations are rejected with `stale_observation`;
a newer contradictory readback may replace prediction/confirmation. `timestampMs`
remains event/display time and is never used for TTL/order. Explicit timestamped
internal observations must supply a local steady order stamp; event time alone
is rejected. Synchronous local observations may omit both stamps. No event sourcing.

#### Built-in observation validation

Generic devices already use `config::accepts` schema checks. Built-ins:

| Kind | Property | Invariant |
| --- | --- | --- |
| Light | `brightness` | integer within effective brightness domain |
| EntranceDoor | `position` | string `OPEN` or `CLOSED` only |

Observation may contradict a prior prediction; it may not violate type/domain.

### `BoundDeviceExecutor`

| Intent | Path |
| --- | --- |
| `Playground` | `MockDeviceExecutor` only — no HTTP, no production store, no connectivity |
| `Production` | Binding → HardwareExecutor → apply `reachability`; on Accepted also Predicted |

Playground virtual simulation remains available for Admin `/api/test-command` and
Apply state preview. Production DeviceStateStore is what `/api/devices/state`
exposes.

### Internal simulated observation (HTTP surface removed in M18.3)

`RuntimeGenerations::observe(StateObservation)` and worker method
`observe_state` remain trusted internal/test seams for supplying Confirmed
observations. M15 originally exposed `POST /api/devices/observe` behind the Admin
mutation guard. [M18.3 removes that public route](m18-production-api-hardening.md):
it returns 404 even with valid Admin headers. A caller-supplied value is not
independent physical evidence, and a local/same-origin guard cannot make it such.
Production HTTP clients use `POST /api/devices/poll`, which obtains values through
the active configured device GET mapping. The underlying `StateObservation` and
DeviceStateStore validation remain available for real polling and isolated tests.

## Stale

```cpp
isStale(property, nowMs, ttlMs = 60s)
```

Uses `observedAtMs` for Confirmed and `updatedAtMs` for Predicted. Tests inject a
deterministic clock — no background scheduler, no sleeps.

## Future telemetry transports

1. Implement a transport-specific poller/subscriber.
2. Map readings into `StateObservation`.
3. Call `DeviceStateStore::applyObservation`.
4. Leave CommandEngine / SafetyGate / GenericAction / LLM untouched.

## Intentionally deferred

Real ESP32/STM32, MQTT, RS-485, CAN, GPIO, heartbeat, event sourcing, durable
telemetry DB, cloud, new Admin authentication beyond the existing local mutation
guard.
