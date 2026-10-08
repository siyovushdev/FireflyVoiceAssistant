#!/usr/bin/env bash
# One appliance process. Model switching is owned by SetupSession/RuntimeModel.
# Production argument parity is enforced by ProductionStartupContract.
# See packaging/firefly-voice-assistant.service and docs/m18-production-startup.md.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [[ $# -ne 1 && $# -ne 2 ]] || [[ $# -eq 2 && "$2" != --new ]]; then
    echo "Usage: $0 STORE [--new]" >&2
    echo '--new initializes an absent configuration with an empty system; existing stores are preserved.' >&2
    exit 2
fi
STORE="$1"
INIT=()
if [[ $# -eq 2 ]]; then
    INIT=(--init-config "$ROOT/config/empty-home.json")
elif [[ ! -f "$STORE/configuration.json" ]]; then
    echo 'No confirmed configuration in STORE. Use --new only to create a new system.' >&2
    exit 2
fi
python3 - <<'PY'
from pathlib import Path
for entry in Path('/proc').iterdir():
    if not entry.name.isdigit():
        continue
    try:
        if 'librkllmrt.so' in (entry/'maps').read_text():
            raise SystemExit(f'RKLLM is already loaded in PID {entry.name}. No second instance was started.')
    except (PermissionError, FileNotFoundError, ProcessLookupError):
        pass
PY
STT="${FIREFLY_STT_MODEL_DIR:-$ROOT/build/stt/model}"
VAD="${FIREFLY_VAD_MODEL:-$ROOT/build/stt/vad-ab/silero_vad.onnx}"
WAKE_PYTHON="${FIREFLY_WAKE_PYTHON:-$ROOT/build/wake-venv/bin/python}"
[[ -d "$STT" && -f "$VAD" && -x "$WAKE_PYTHON" ]] || { echo 'Local STT/VAD/wake dependencies are missing; see docs/dialogue-setup.md.' >&2; exit 1; }
exec "$ROOT/build/firefly-demo" --store "$STORE" "${INIT[@]}" \
    --model /home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm \
    --admin-bind 127.0.0.1 --admin-port "${FIREFLY_ADMIN_PORT:-8080}" \
    --stt-model-dir "$STT" --voice --vad-model "$VAD" \
    --audio-device 'plughw:CARD=Array,DEV=0' --wake-enabled true \
    --wake-python "$WAKE_PYTHON" --wake-model "$ROOT/assets/wake/amina-v3.tflite" \
    --wake-cutoff 0.99 --wake-window 7 \
    --playback-device 'plughw:CARD=rockchipes8388,DEV=0'
