# Firefly V1 Prototype Integration

Post-M17 baseline. Proves the **product pipeline** voice → action → production
execution → Predicted device state using the existing HTTP hardware path and a
**loopback simulated device** (not ESP32).

## Full user path

```text
Voice / Admin text
    ↓
STT (voice) or typed text (Admin / tests)
    ↓
DeterministicParser / RKLLM fallback
    ↓
SafetyGate + ActionValidator → GenericAction
    ↓
ExecutionIntent::Production
    ↓
BindingResolver
    ↓
HttpHardwareExecutor  →  scripts/v1-mock-device.py (127.0.0.1:8765)
    ↓
ExecutionStatus::Accepted
    ↓
DeviceStateStore Predicted (not Confirmed)
```

Frozen light protocol (no `power_on` / `power_off` IDs):

| Phrase | GenericAction |
| --- | --- |
| Включи свет на кухне | `action_set_brightness` / `device_kitchen_light` / brightness **100** |
| Выключи свет на кухне | brightness **0** |
| Поставь свет на кухне на 40 процентов | brightness **40** |

## Demo configuration

`config/demo/v1-kitchen.json` — schema v2, full M4 rooms + **Kitchen Light** with
one HTTP binding:

```text
POST http://127.0.0.1:8765/brightness  argument=brightness
```

## Repeatable demo

```bash
./scripts/v1-demo-run.sh
```

See also `docs/v1-demo-scenario.md` (voice smoke checklist, Admin/curl steps, ESP32 cutover).

### Manual Admin / mock device (optional)

```bash
# 1) Simulated device (existing HttpHardwareExecutor sink)
python3 scripts/v1-mock-device.py

# 2) Init store from V1 demo config (separate terminal)
mkdir -p /tmp/firefly-v1-store && chmod 700 /tmp/firefly-v1-store
./build/firefly-demo \
  --store /tmp/firefly-v1-store \
  --init-config config/demo/v1-kitchen.json \
  --model /home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm \
  --admin-bind 127.0.0.1 --admin-port 8080

# 3) Admin → Command playground → enable «Production (binding)» → Execute
#    or typed POST:
curl -s http://127.0.0.1:8080/api/test-command -H 'Content-Type: application/json' \
  -d '{"text":"Включи свет на кухне","execute":true,"production":true}'
```

Voice (optional, real mic/STT/RKLLM): add `--voice --stt-model-dir … --vad-model …`.
Voice always uses `ExecutionIntent::Production`. Production voice now defaults to
[Amina wake gating](v1-amina-wake.md); say «Амина» before each command.

## Admin result copy (M15)

Success (Predicted only):

* Результат: Accepted
* Состояние: Ожидается: brightness N

Never claim «Свет включён» / «устройство включено» from Predicted alone.

Failure examples: `binding_missing`, transport errors — no DeviceStateStore mutation.

## Deterministic tests

`V1PrototypePipeline` (`tests/execution/V1PrototypeTests.cpp`):

* text-as-STT → GenericAction for the three kitchen phrases;
* Production + FakeHttpServer → Accepted + Predicted;
* binding missing → Failed, no mutation.

No second Qwen instance; FakeModel only.

## Intentionally out of scope

ESP32, MQTT, RS-485, CAN, GPIO, new LLM/STT, wake-word redesign, Confirmed
without separate observe/readback.
