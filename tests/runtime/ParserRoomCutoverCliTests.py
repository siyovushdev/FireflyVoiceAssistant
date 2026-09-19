"""Migrated lookup admission at real application entry points, without NPU/audio."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile

app = Path(sys.argv[1]).resolve()
root = Path(sys.argv[2]).resolve()
voice = len(sys.argv) == 4 and sys.argv[3] == "--voice"
sample = json.loads((root / "config/current-m4.json").read_text())
checks = 0
with tempfile.TemporaryDirectory(prefix="firefly-parser-cutover-") as directory:
    directory = Path(directory)
    path = directory / "config.json"
    prompt = directory / "prompt.txt"
    prompt.write_text("frozen")
    args = [str(app), "--config", str(path), "--model", "unused", "--system-prompt", str(prompt)]
    if voice:
        args += ["--voice", "--stt-model-dir", "unused", "--vad-model", str(directory / "absent-vad.onnx")]
    # All three variants are storage-valid configurations with changed locations.
    variants = []
    removed = copy.deepcopy(sample)
    next(room for room in removed["rooms"] if room["id"] == "room_living")["speech"]["locations"].remove("в зале")
    variants.append(removed)
    added = copy.deepcopy(sample)
    added["rooms"][0]["speech"]["locations"].append("в кухне")
    variants.append(added)
    moved = copy.deepcopy(sample)
    living = next(room for room in moved["rooms"] if room["id"] == "room_living")["speech"]
    bedroom = next(room for room in moved["rooms"] if room["id"] == "room_bedroom")["speech"]
    living["locations"].remove("в зале")
    bedroom["locations"].append("в зале")
    for alias in ("зал", "зале", "зала", "залу", "залом"):
        living["mentions"].remove(alias)
        bedroom["mentions"].append(alias)
    variants.append(moved)
    for candidate, expected in zip(variants, ("U", "L|G|40", "L|B|40")):
        path.write_text(json.dumps(candidate, ensure_ascii=False))
        result = subprocess.run(args, input="в зале свет 40\n", capture_output=True, text=True, timeout=15)
        assert "CONFIG_RUNTIME" in result.stderr and "legacy_shadow" not in result.stderr, result.stderr
        if voice:
            assert result.returncode == 1 and result.stdout == "", result
            assert result.stderr.index("PARSER_ROOMS") < result.stderr.index("Loading VAD"), result.stderr
            assert "Missing speech model file:" in result.stderr, result.stderr
        else:
            assert result.returncode == 0 and result.stdout == expected + "\n", result
            assert "READY" in result.stderr, result.stderr
        checks += 1
    # Current configuration passes the new admission check in both entry points.
    path.write_text(json.dumps(sample, ensure_ascii=False))
    result = subprocess.run(args, input="на кухне поставь свет на 40 процентов\nв зале свет 40\n", capture_output=True, text=True, timeout=15)
    assert result.stderr.count("PARSER_ROOMS authoritative=config action_model=generic") == 1, result.stderr
    if voice:
        assert result.returncode == 1 and result.stdout == "", result
        assert result.stderr.index("PARSER_ROOMS") < result.stderr.index("Loading VAD"), result.stderr
        assert "Missing speech model file:" in result.stderr, result.stderr
    else:
        assert result.returncode == 0 and result.stdout == "L|K|40\nL|G|40\n", result
        assert result.stderr.index("PARSER_ROOMS") < result.stderr.index("Loading RKLLM"), result.stderr
    checks += 1
print(f"ParserRoomCutover {'voice' if voice else 'text'}: {checks} startup cases passed; configured semantic resolution, no legacy veto")
