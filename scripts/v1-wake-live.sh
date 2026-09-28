#!/usr/bin/env bash
# Single runtime, fresh loopback-only V1 store. No physical device binding.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
for executable in firefly-demo firefly-assistant firefly-admin-runtime firefly-regression; do
    if pgrep -f "(^|/)${executable}( |$)" >/dev/null; then
        echo "Stop the existing ${executable} before live acceptance." >&2
        exit 1
    fi
done
if pgrep -x arecord >/dev/null || pgrep -f '[/]amina_v3_shadow.py' >/dev/null; then
    echo 'Stop the existing audio consumer before live acceptance.' >&2
    exit 1
fi
# Detect any already-loaded real RKLLM runtime, including differently named harnesses.
python3 - <<'PY'
from pathlib import Path
for entry in Path('/proc').iterdir():
    if not entry.name.isdigit():
        continue
    try:
        if 'librkllmrt.so' in (entry / 'maps').read_text():
            raise SystemExit(f'RKLLM already loaded in PID {entry.name}; stop it first')
    except (PermissionError, ProcessLookupError, FileNotFoundError):
        pass
PY
WAKE_PYTHON="${FIREFLY_WAKE_PYTHON:-$ROOT/build/wake-venv/bin/python}"
"$WAKE_PYTHON" -c 'import ai_edge_litert.interpreter, pymicro_features, numpy'
RUN_DIR="$(mktemp -d "$ROOT/build/wake-live.XXXXXX")"
mkdir -m 700 "$RUN_DIR/store"
mock_pid=""
runtime_pid=""
cleanup() {
    trap - EXIT INT TERM
    if [[ -n "$runtime_pid" ]]; then kill -TERM "$runtime_pid" 2>/dev/null || true; wait "$runtime_pid" || true; fi
    if [[ -n "$mock_pid" ]]; then kill -TERM "$mock_pid" 2>/dev/null || true; wait "$mock_pid" || true; fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
python3 -u scripts/v1-mock-device.py --ready-file "$RUN_DIR/mock-ready.json" >"$RUN_DIR/http-mock.log" 2>&1 &
mock_pid=$!
# Only our successfully bound mock may signal readiness. A connect to port 8765
# could succeed against an unrelated listener before our child reports bind failure.
python3 - "$mock_pid" "$RUN_DIR/mock-ready.json" <<'PYREADY'
import json, sys, time
from pathlib import Path
pid = int(sys.argv[1])
ready = Path(sys.argv[2])
for _ in range(50):
    try:
        state = Path(f'/proc/{pid}/stat').read_text().split(') ')[1].split()[0]
    except FileNotFoundError:
        state = 'gone'
    if state in ('Z', 'gone'):
        raise SystemExit('Owned mock device failed to start; inspect http-mock.log (port 8765 may be occupied)')
    if ready.exists():
        try:
            identity = json.loads(ready.read_text())
        except json.JSONDecodeError:
            time.sleep(.1)
            continue
        if identity != {'pid': pid, 'host': '127.0.0.1', 'port': 8765}:
            raise SystemExit('Unexpected owned mock identity')
        break
    time.sleep(.1)
else:
    raise SystemExit('Owned mock device startup timed out')
PYREADY
printf 'Acceptance artifacts: %s\n' "$RUN_DIR"
printf 'Wait for VOICE_STATE WAITING_WAKE. Say Амина; after LISTENING_COMMAND say one command.\n'
printf 'Watch states: tail -f %s/voice.log\n' "$RUN_DIR"
./build/firefly-demo \
    --store "$RUN_DIR/store" --init-config config/demo/v1-kitchen.json \
    --model /home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm \
    --admin-bind 127.0.0.1 --admin-port 8080 \
    --voice --audio-device 'plughw:CARD=Array,DEV=0' \
    --stt-model-dir "$ROOT/build/stt/model" \
    --vad-model "$ROOT/build/stt/vad-ab/silero_vad.onnx" \
    --wake-enabled true --wake-python "$WAKE_PYTHON" \
    --wake-model "$ROOT/assets/wake/amina-v3.tflite" \
    --wake-cutoff 0.99 --wake-window 7 --command-listen-timeout-ms 10000 \
    >"$RUN_DIR/voice.log" 2>&1 &
runtime_pid=$!
wait "$runtime_pid"
