#!/usr/bin/env bash
# One appliance process for the relocatable prefix.
# Production supervision is the systemd unit, which execs firefly-demo directly.
# Argument parity is enforced by ProductionStartupContract.
# See packaging/firefly-voice-assistant.service and docs/m18-relocatable-production-packaging.md.
set -euo pipefail
if [[ $# -ne 1 && $# -ne 2 ]] || [[ $# -eq 2 && "$2" != --new ]]; then
    echo "Usage: $0 STORE [--new]" >&2
    echo '--new initializes an absent configuration with an empty system; existing stores are preserved.' >&2
    exit 2
fi
PREFIX="${FIREFLY_PREFIX:-/opt/firefly/voice-assistant}"
RELEASE="$PREFIX/current"
STORE="$1"
MODEL="$PREFIX/models/runtime/Qwen3-4B_W8A8_RK3588_3core.rkllm"
ADMIN_MODEL="$PREFIX/models/admin/Qwen3-4B_W8A8_CTX4096_RK3588_3core.rkllm"
STT="$PREFIX/models/stt"
VAD="$PREFIX/models/vad/silero_vad.onnx"
WAKE_PYTHON="$RELEASE/share/firefly/wake/python/bin/python"
WAKE_MODEL="$RELEASE/share/firefly/wake/amina-v3.tflite"
INIT_CONFIG="$RELEASE/share/firefly/config/empty-home.json"
BIN="$RELEASE/bin/firefly-demo"
INIT=()
if [[ $# -eq 2 ]]; then
    [[ -f "$INIT_CONFIG" ]] || { echo "Missing empty init config: $INIT_CONFIG" >&2; exit 1; }
    INIT=(--init-config "$INIT_CONFIG")
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
[[ -x "$BIN" && -f "$MODEL" && -f "$ADMIN_MODEL" && -d "$STT" && -f "$VAD" && -x "$WAKE_PYTHON" && -f "$WAKE_MODEL" ]] || {
    echo "Relocatable runtime dependencies are missing under $PREFIX. See docs/m18-relocatable-production-packaging.md." >&2
    exit 1
}
exec "$BIN" --store "$STORE" "${INIT[@]}" \
    --model "$MODEL" \
    --admin-model "$ADMIN_MODEL" \
    --admin-bind 127.0.0.1 --admin-port "${FIREFLY_ADMIN_PORT:-8080}" \
    --stt-model-dir "$STT" --voice --vad-model "$VAD" \
    --audio-device 'plughw:CARD=Array,DEV=0' --wake-enabled true \
    --wake-python "$WAKE_PYTHON" --wake-model "$WAKE_MODEL" \
    --wake-cutoff 0.99 --wake-window 7 \
    --playback-device 'plughw:CARD=rockchipes8388,DEV=0'
