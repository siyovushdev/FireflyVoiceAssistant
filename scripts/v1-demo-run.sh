#!/usr/bin/env bash
# Firefly V1 demo — one-shot kitchen Production pipeline (no RKLLM / no ESP32).
#
# Uses FakeModel + loopback FakeHttpServer inside V1PrototypePipeline.
# Same Binding → HttpHardwareExecutor path that later points at ESP32.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BIN="${ROOT}/build/src/admin/v1_prototype_tests"

if [[ ! -x "$BIN" ]] || [[ "$BIN" -ot "${ROOT}/tests/execution/V1PrototypeTests.cpp" ]]; then
  echo "Building v1_prototype_tests…"
  cmake -S . -B build -DRKLLM_ROOT="${RKLLM_ROOT:-/home/firefly/demo_Linux_aarch64}" >/dev/null
  cmake --build build -j"$(nproc)" --target v1_prototype_tests
fi

echo "=== Firefly V1 Demo & Voice Validation (deterministic) ==="
echo "Config: config/demo/v1-kitchen.json"
echo "Device: Kitchen Light  ·  path: Production → HTTP mock → Predicted"
echo
"$BIN" "$ROOT"
echo
echo "Primary scenario (canonical):"
echo "  Input:  Поставь свет на кухне на 40 процентов"
echo "  Action: device_kitchen_light / action_set_brightness / brightness=40"
echo "  Exec:   Accepted"
echo "  State:  Predicted brightness=40"
echo
echo "Live voice smoke (optional, real STT/mic — see docs/v1-demo-scenario.md):"
echo "  1) python3 scripts/v1-mock-device.py"
echo "  2) firefly-demo --store … --init-config config/demo/v1-kitchen.json --voice …"
echo "  3) Speak: Включи / Выключи / Поставь … на сорок процентов"
