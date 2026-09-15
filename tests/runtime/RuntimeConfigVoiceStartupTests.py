"""Voice assembly startup failures only: never opens models or an audio device."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

app, root = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
with tempfile.TemporaryDirectory(prefix="firefly-voice-config-") as directory:
    path = Path(directory) / "invalid.json"
    args = [str(app), "--voice", "--model", "unused", "--system-prompt", "unused",
            "--stt-model-dir", "unused", "--vad-model", "unused", "--config", str(path)]
    candidate = json.loads((root / "config/current-m4.json").read_text())
    candidate["safety"]["skip_safety"] = True
    variants = [None, "{", json.dumps(candidate)]
    for variant in variants:
        if variant is not None:
            path.write_text(variant)
        result = subprocess.run(args, capture_output=True, text=True, timeout=10)
        assert result.returncode == 1 and result.stdout == "", result
        assert "Voice error:" in result.stderr, result.stderr
        for stage in ("Loading VAD", "Loading offline STT", "CONFIG_SHADOW", "LISTENING", "Loading RKLLM", "Loading model", "Building system KV"):
            assert stage not in result.stderr, result.stderr
    # A valid snapshot reaches the next startup stage, where a deliberately
    # absent speech model stops execution before any vendor model/capture opens.
    path.write_text((root / "config/current-m4.json").read_text())
    args[args.index("--vad-model") + 1] = str(Path(directory) / "absent-vad.onnx")
    result = subprocess.run(args, capture_output=True, text=True, timeout=10)
    assert result.returncode == 1 and result.stdout == "", result
    assert "mismatches=0 authoritative=legacy" in result.stderr, result.stderr
    assert result.stderr.index("CONFIG_SHADOW") < result.stderr.index("Loading VAD"), result.stderr
    assert "Missing speech model file:" in result.stderr, result.stderr
    assert "Loading offline STT" not in result.stderr and "LISTENING" not in result.stderr, result.stderr
print("4 voice startup checks: invalid config stops before speech; valid config precedes VAD")
