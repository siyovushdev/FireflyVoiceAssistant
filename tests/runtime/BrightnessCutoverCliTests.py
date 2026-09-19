"""Range cutover through real text/voice assembly; no NPU, capture or model loads."""
import copy
import json
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time
import unittest

APP = Path(sys.argv[1]).resolve()
ROOT = Path(sys.argv[2]).resolve()
VOICE = len(sys.argv) == 4 and sys.argv[3] == "--voice"
ORIGINAL = (ROOT / "config/current-m4.json").read_bytes()
SAMPLE = json.loads(ORIGINAL)
sys.argv = [sys.argv[0]]


class BrightnessCutoverCliTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="firefly-brightness-cutover-")
        self.addCleanup(self.scratch.cleanup)
        self.directory = Path(self.scratch.name)
        self.config = self.directory / "config.json"
        self.args = [str(APP), "--config", str(self.config), "--model", "unused"]
        if VOICE:
            self.args += ["--voice", "--stt-model-dir", "unused", "--vad-model",
                          str(self.directory / "absent-vad.onnx")]

    def tearDown(self):
        self.assertEqual((ROOT / "config/current-m4.json").read_bytes(), ORIGINAL)

    def configured(self, minimum, maximum):
        candidate = copy.deepcopy(SAMPLE)
        parameter = next(action for action in candidate["actions"]
                         if action["operation"] == "light.set_brightness.v1")["parameters"][0]
        parameter.update(minimum=minimum, maximum=maximum)
        return candidate

    def run_app(self, candidate, commands="на кухне свет 40\n"):
        self.config.write_text(json.dumps(candidate, ensure_ascii=False))
        return subprocess.run(self.args, input=commands, capture_output=True, text=True,
                              cwd=self.directory, timeout=15)

    def assert_ready(self, result, outputs):
        self.assertEqual(result.stderr.count("CONFIG_RUNTIME"), 1, result.stderr)
        self.assertIn("action_model=generic domain_authority=config", result.stderr)
        self.assertIn("PARSER_ROOMS authoritative=config action_model=generic", result.stderr)
        if VOICE:
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertEqual(result.stdout, "")
            self.assertIn("Missing speech model file:", result.stderr)
            self.assertLess(result.stderr.index("CONFIG_RUNTIME"), result.stderr.index("Loading VAD"))
            for marker in ("Loading offline STT", "Loading RKLLM", "LISTENING"):
                self.assertNotIn(marker, result.stderr)
        else:
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), outputs)
            self.assertLess(result.stderr.index("CONFIG_RUNTIME"), result.stderr.index("Loading RKLLM"))

    def test_current_and_remapped_range_boundaries(self):
        for minimum, maximum in ((0,100), (10,90), (50,50)):
            commands, expected = [], []
            for location, code in (("на кухне","K"), ("в спальне","B"), ("в гостиной","G"),
                                   ("в зале","G"), ("в коридоре","C")):
                for value in (-1,0,1,9,10,11,40,50,89,90,91,99,100,101):
                    commands.append(f"{location} свет {value}")
                    expected.append(f"L|{code}|{value}" if minimum <= value <= maximum else "U")
                commands.extend((f"выключи свет {location}", f"{location} свет на максимум", f"{location} свет наполовину"))
                expected.extend(f"L|{code}|{value}" if minimum <= value <= maximum else "U" for value in (0,100,50))
            candidate = self.configured(minimum, maximum)
            result = self.run_app(candidate, "\n".join(commands) + "\n")
            self.assert_ready(result, expected)
            if minimum and not VOICE:
                self.assertIn("Rejected: Action outside configured domain", result.stderr)
                self.assertNotIn("legacy_shadow", result.stderr)

    def test_invalid_capability_fails_before_models(self):
        for invalid in ("missing_minimum", "negative", "too_large", "reversed", "wrong_parameter",
                        "missing_action", "unknown_target", "wrong_kind"):
            candidate = self.configured(0,100)
            parameter = candidate["actions"][0]["parameters"][0]
            if invalid == "missing_minimum":
                del parameter["minimum"]
            elif invalid == "negative":
                parameter["minimum"] = -1
            elif invalid == "too_large":
                parameter["maximum"] = 101
            elif invalid == "reversed":
                parameter.update(minimum=90, maximum=10)
            elif invalid == "wrong_parameter":
                parameter["id"] = "temperature"
            elif invalid == "missing_action":
                candidate["devices"][0]["action_ids"] = []
            elif invalid == "unknown_target":
                candidate["compatibility"]["light_targets"]["K"] = "unknown_device"
            elif invalid == "wrong_kind":
                candidate["devices"][0]["kind"] = "entrance_door"
            result = self.run_app(candidate)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertEqual(result.stdout, "" if VOICE else "U\n")
            self.assertIn("Configuration startup failed", result.stderr)
            for marker in ("CONFIG_RUNTIME", "READY", "LISTENING", "Loading VAD", "Loading RKLLM"):
                self.assertNotIn(marker, result.stderr)

    def test_configured_locations_have_no_legacy_veto(self):
        candidate = self.configured(10,90)
        living = next(room for room in candidate["rooms"] if room["id"] == "room_living")
        living["speech"]["locations"].remove("в зале")
        result = self.run_app(candidate)
        self.assert_ready(result, ["L|K|40"])
        self.assertNotIn("shadow mismatch", result.stderr)

    def test_permission_fields_remain_observational(self):
        candidate = self.configured(0,100)
        for device in candidate["devices"]:
            device["enabled"] = False
        for action in candidate["actions"]:
            action["enabled"] = False
        candidate["safety"]["restrictions"] = [{"id":"deny_kitchen", "name":"Запрет", "kind":"deny_action",
                                                 "device_id":"device_kitchen_light", "action_id":"action_set_brightness"}]
        result = self.run_app(candidate, "на кухне свет 40\nОткрой входную дверь\n")
        self.assert_ready(result, ["L|K|40", "D"])

    @unittest.skipIf(VOICE, "Snapshot request checks use text entrypoint; voice stops before absent VAD model")
    def test_snapshot_range_is_retained_until_restart(self):
        self.config.write_text(json.dumps(self.configured(10,90)))
        process = subprocess.Popen(self.args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, bufsize=0, cwd=self.directory)
        try:
            deadline = time.monotonic() + 15
            startup = b""
            while not startup.endswith(b"READY\n"):
                remaining = deadline - time.monotonic()
                self.assertGreater(remaining, 0, startup)
                self.assertTrue(select.select([process.stderr], [], [], remaining)[0], startup)
                byte = process.stderr.read(1)
                self.assertTrue(byte, startup)
                startup += byte
            self.config.write_text(json.dumps(SAMPLE))
            stdout, stderr = process.communicate("на кухне свет 0\nна кухне свет 10\nна кухне свет 90\nна кухне свет 100\n".encode(), timeout=15)
            self.assertEqual(process.returncode, 0, stderr)
            self.assertEqual(stdout, b"U\nL|K|10\nL|K|90\nU\n")
            self.assertEqual((startup + stderr).count(b"CONFIG_RUNTIME"), 1)
            self.assert_ready(self.run_app(SAMPLE, "на кухне свет 0\nна кухне свет 100\n"), ["L|K|0", "L|K|100"])
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate()


if __name__ == "__main__":
    unittest.main()
