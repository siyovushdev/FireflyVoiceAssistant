# Firefly V1 Demo Scenario

Stable kitchen-light demonstration after V1 Prototype Integration.
Architecture M14–M17 unchanged.

## Scenario

| Field | Value |
| --- | --- |
| Location | Кухня (`room_kitchen`) |
| Device | Kitchen Light (`device_kitchen_light`) |
| Action | `action_set_brightness` (brightness model only) |
| Binding | HTTP → `127.0.0.1:8765/brightness` (mock device) |

Brightness meanings:

| Value | Meaning |
| --- | --- |
| 100 | on |
| 0 | off |
| 40 | partial |

## How to run (deterministic, no Qwen)

```bash
./scripts/v1-demo-run.sh
```

This runs `V1PrototypePipeline`: text → GenericAction → Production →
HttpHardwareExecutor → loopback mock → Accepted → Predicted.

Canonical command:

```text
Input:  Поставь свет на кухне на 40 процентов

GenericAction:
  targetId = device_kitchen_light
  actionId = action_set_brightness
  brightness = 40

Execution: Accepted
State:     Predicted brightness = 40
```

## Live Admin / mock device (optional)

```bash
# Terminal A — same HttpHardwareExecutor sink later replaced by ESP32
python3 scripts/v1-mock-device.py

# Terminal B
mkdir -p /tmp/firefly-v1-store && chmod 700 /tmp/firefly-v1-store
./build/firefly-demo \
  --store /tmp/firefly-v1-store \
  --init-config config/demo/v1-kitchen.json \
  --model /home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm \
  --admin-bind 127.0.0.1 --admin-port 8080
```

In Admin Command Playground: enable **Production (binding)** → Execute.

Or:

```bash
curl -s http://127.0.0.1:8080/api/test-command \
  -H 'Content-Type: application/json' \
  -d '{"text":"Поставь свет на кухне на 40 процентов","execute":true,"production":true}'
```

## Expected user-visible result

```text
Команда:    Поставь свет на кухне на 40 процентов
Устройство: Kitchen Light
Результат:  Accepted
Состояние:  Ожидается: brightness 40
```

Do **not** say «Свет включён» from Predicted alone (M15).

## Voice smoke checklist (do not change STT/LLM)

Prepare live phrases (mic + STT + optional RKLLM fallback):

1. `Включи свет на кухне` → brightness 100 (deterministic)  
2. `Выключи свет на кухне` → brightness 0 (deterministic)  
3. `Поставь свет на кухне на сорок процентов` → brightness 40  

**Demo runner / typed path** uses digits: `Поставь свет на кухне на 40 процентов` (fast-path).

**Limitation:** DeterministicParser matches **digits** (`40`), not word numerals (`сорок`).
Frozen eval WAV transcript `на кухне поставь свет на сорок процентов` needs **RKLLM fallback**
(or STT emitting digits). Record live outcomes only — do not retune STT/parser here.

| Phrase | STT text OK? | Path (fast/LLM) | GenericAction OK? | Production Accepted? | Notes |
| --- | --- | --- | --- | --- | --- |
| Включи… | | | | | |
| Выключи… | | | | | |
| …сорок процентов | | | | | often LLM |
| …40 процентов (typed) | n/a | fast | yes | yes | `./scripts/v1-demo-run.sh` |

Other limits: room-less `включи свет` → U; no wake-word; do not start a second Qwen while demo holds the NPU.

## Pipeline

```text
User command (text or STT)
  → DeterministicParser / RKLLM
  → SafetyGate
  → ExecutionIntent::Production
  → BindingResolver
  → HttpHardwareExecutor
  → Mock HTTP (127.0.0.1)   ← replace URL with ESP32 later
  → ExecutionResult Accepted
  → DeviceStateStore Predicted
```

## ESP32 cutover

Replace binding URL/host in `config/demo/v1-kitchen.json` (or Admin Connections)
with the ESP32 HTTP endpoint. Keep `argument: brightness`. No new executor.
Confirmed knowledge still requires separate observe/readback (not part of V1 demo).
