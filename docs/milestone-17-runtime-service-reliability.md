# Milestone 17 — Runtime Service & Reliability

Resume baseline: Milestone 16 (`d6356b0`). This milestone productizes Firefly as a
**single-process Linux appliance** supervised by systemd.

## systemd vs Firefly

| Responsibility | Owner |
| --- | --- |
| Autostart after boot | **systemd** |
| Restart on process crash + backoff / start limits | **systemd** |
| SIGTERM delivery + stop timeout | **systemd** |
| journald capture of stdout/stderr | **systemd** |
| Startup phases, health model, config LKG recovery | **Firefly** |
| Subsystem isolation (LLM/STT fail ≠ process exit) | **Firefly** |
| Graceful reject of new work while stopping | **Firefly** |
| Custom restart loop / multi-service orchestration | **Neither (not in M17)** |

Unit template: `packaging/firefly-voice-assistant.service` (M18.1 now enables the
complete voice/STT/VAD/Amina stack). See [production startup and completed M18.1
owner acceptance (PASS)](m18-production-startup.md), including real reboot/autostart
and the XVF3800 USB firmware prerequisite: 2.0.10 or newer required for the validated
appliance (2.0.10 tested; 2.0.6 rejected). M18.2 remains deferred.
**Do not** enable/install automatically from build or tests.

### Watchdog decision

M17 ships `Type=simple` **without** `sd_notify` / systemd watchdog. Heartbeating
the main event loop cleanly (without a false-live side thread during RKLLM hangs)
is deferred. Autostart + `Restart=on-failure` remain mandatory.

## Startup lifecycle

```text
process_started
→ configuration_loading   (reconcile store; load current or LKG)
→ http_listening          (Admin HTTP bound — available even if model fails)
→ runtime_initializing    (Apply generation)
→ ready | degraded_startup | recovery_mode
```

* Admin HTTP binds and prints `DEMO_LISTEN` **before** Admin Copilot STT
  construction and **before** RKLLM Apply (`initializeRuntime` on a service thread).
* `RuntimeModel` loads RKLLM lazily on first `useContext` during Apply.
* STT/LLM init exceptions update health to DEGRADED; they do not abort the process.

Logged with stable `STARTUP …` / `HEALTH …` / `SHUTDOWN …` lines on stderr
(journald).

## Health model

### Overall

| Value | Meaning |
| --- | --- |
| `starting` | Boot not finished |
| `ready` | configuration + runtime + admin ready; optional STT/LLM ready |
| `degraded` | Admin up; one or more subsystems failed **or** config recovered from LKG |
| `failed` | Cannot provide safe Admin/recovery (rare; e.g. admin failed) |

LLM/STT failure alone → **degraded**, not failed.

### Subsystems

`configuration`, `runtime`, `admin`, `stt`, `llm`, `hardware_execution`, `wake`
(and `acknowledgement` when playback is exercised). Missing microphone/wake
leaves Admin available with degraded health; automatic microphone recovery is
deferred to M18.2.

Exposed on `GET /api/status` as:

```json
{
  "health": {
    "overall": "degraded",
    "phase": "degraded_startup",
    "stopping": false,
    "config_recovered": true,
    "subsystems": { "...": { "status": "ready|failed|…", "error": "…" } }
  }
}
```

Existing status fields are preserved.

## Config / Last Known Good

**LKG** = configuration that:

1. passed `ConfigValidator` / `RuntimeGeneration` admission;
2. successfully **Apply**’d;
3. was recorded via `ConfigStore::recordLastKnownGood`.

Marker files in the store directory:

* `last_known_good.json` — `{revision,hash}`
* `last_known_good_snapshot.json` — durable canonical catalog bytes

LKG does **not** write into `backups/` (that directory remains save/restore
revision history only). Older markers that only pointed at a backup still resolve.

Boot policy:

```text
try current
  → admit OK → Apply → record LKG → READY/DEGRADED by model
  → fail → try LKG
       → admit OK → Apply LKG (store unchanged) → DEGRADED + config_recovered
       → fail → recovery mode (Admin up, execution disabled)
```

No silent fabrication of physical DeviceState. After process restart, M15
DeviceStateStore remains cold-start **Unknown**.

## Shutdown

On SIGTERM/SIGINT (`DemoMain`):

1. `Api::beginShutdown()` → health stopping;
2. HTTP loop exits;
3. `RuntimeWorker::stop()` → `RuntimeGenerations::stop` rejects new work;
4. service thread joined;
5. `SHUTDOWN complete`.

systemd `TimeoutStopSec=45`. No new hardware commands after stop begins.

## Crash / restart

systemd restarts the process. In-memory Predicted/Confirmed device knowledge is
**not** restored. Durable config/LKG is.

## Installation (manual)

```bash
sudo install -d -m 700 -o firefly -g firefly /var/lib/firefly/store
sudo install -m 644 packaging/firefly-voice-assistant.service /etc/systemd/system/
# Build with ADMIN/ASSISTANT/VOICE ON and provision the offline dependencies.
# Follow docs/m18-production-startup.md; critical arguments are regression-tested.
sudo systemctl daemon-reload
sudo systemctl start firefly-voice-assistant
# Enable boot startup only after the service-start acceptance passes:
# sudo systemctl enable firefly-voice-assistant
systemctl status firefly-voice-assistant
journalctl -u firefly-voice-assistant -f
```

Rollback:

```bash
sudo systemctl disable --now firefly-voice-assistant
sudo rm /etc/systemd/system/firefly-voice-assistant.service
sudo systemctl daemon-reload
```

## Intentionally deferred

`sd_notify` watchdog, multi-service split, MQTT/GPIO, secrets vault, cloud,
second RKLLM instance, automatic permanent enable from CI.
