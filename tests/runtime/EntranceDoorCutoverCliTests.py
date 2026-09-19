"""Entrance identity admission through actual text/voice assembly, without NPU inference."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

APP = Path(sys.argv[1]).resolve()
ROOT = Path(sys.argv[2]).resolve()
VOICE = len(sys.argv) == 4 and sys.argv[3] == "--voice"
ORIGINAL = (ROOT / "config/current-m4.json").read_bytes()
SAMPLE = json.loads(ORIGINAL)
sys.argv = [sys.argv[0]]


class EntranceDoorCutoverCliTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="firefly-entrance-cutover-")
        self.addCleanup(self.scratch.cleanup)
        self.directory = Path(self.scratch.name)
        self.config = self.directory / "config.json"
        self.args = [str(APP), "--config", str(self.config), "--model", "unused"]
        if VOICE:
            self.args += ["--voice", "--stt-model-dir", "unused", "--vad-model",
                          str(self.directory / "absent-vad.onnx")]

    def tearDown(self):
        self.assertEqual((ROOT / "config/current-m4.json").read_bytes(), ORIGINAL)

    def run_app(self, candidate, commands="открой входную дверь\n"):
        self.config.write_text(json.dumps(candidate, ensure_ascii=False))
        return subprocess.run(self.args, input=commands, capture_output=True, text=True,
                              cwd=self.directory, timeout=15)

    def door(self, candidate):
        return next(device for device in candidate["devices"]
                    if device["id"] == candidate["compatibility"]["door_target_id"])

    def assert_ready(self, result, outputs):
        self.assertEqual(result.stderr.count("CONFIG_RUNTIME"), 1, result.stderr)
        self.assertIn("action_model=generic domain_authority=config", result.stderr)
        self.assertIn("PARSER_ROOMS authoritative=config action_model=generic", result.stderr)
        self.assertNotIn("legacy_shadow", result.stderr)
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

    def test_door_safety_matrix_and_valid_metadata_edit(self):
        commands = ["дверь", "открой дверь", "открой входную дверь", "открыть дверь на входе",
                    "ОТКРОЙ дверь НА\tВХОДЕ!", "Открой дверь в спальню", "Закрой входную дверь",
                    "Разблокируй входную дверь", "открытвер на входе", "открой дверь на другом входе",
                    "На кухне выключи свет на 50 процентов", "на кухне свет 40"]
        expected = ["U", "U", "D", "D", "D", "U", "U", "U", "U", "U", "U", "L|K|40"]
        for name in ("Входная дверь", "Главный вход — тест", "дверь"):
            candidate = copy.deepcopy(SAMPLE)
            self.door(candidate)["name"] = name
            candidate["devices"].reverse()
            self.assert_ready(self.run_app(candidate, "\n".join(commands) + "\n"), expected)

    def test_invalid_target_fails_before_model_initialization(self):
        for defect in ("missing_mapping", "unknown_target", "missing_device", "light_target", "new_id",
                       "missing_action", "wrong_room", "empty_name", "unknown_action", "invented_policy"):
            candidate = copy.deepcopy(SAMPLE)
            door = self.door(candidate)
            if defect == "missing_mapping":
                del candidate["compatibility"]["door_target_id"]
            elif defect == "unknown_target":
                candidate["compatibility"]["door_target_id"] = "unknown_door"
            elif defect == "missing_device":
                candidate["devices"].remove(door)
            elif defect == "light_target":
                candidate["compatibility"]["door_target_id"] = "device_kitchen_light"
            elif defect == "new_id":
                door["id"] = "device_other_entrance"
                candidate["compatibility"]["door_target_id"] = door["id"]
            elif defect == "missing_action":
                door["action_ids"] = []
            elif defect == "wrong_room":
                door["room_id"] = "room_kitchen"
            elif defect == "empty_name":
                door["name"] = ""
            elif defect == "unknown_action":
                candidate["compatibility"]["door_action_id"] = "unknown_action"
            elif defect == "invented_policy":
                candidate["safety"]["require_explicit_entrance"] = False
            result = self.run_app(candidate)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertEqual(result.stdout, "" if VOICE else "U\n")
            self.assertIn("Configuration startup failed", result.stderr)
            for marker in ("CONFIG_RUNTIME", "READY", "LISTENING", "Loading VAD", "Loading RKLLM"):
                self.assertNotIn(marker, result.stderr)

    def test_extra_entity_cannot_replace_selected_target(self):
        candidate = copy.deepcopy(SAMPLE)
        extra = copy.deepcopy(self.door(candidate))
        extra.update(id="device_aaa_other_entrance", name="Иной вход (тест)")
        candidate["devices"].insert(0, extra)
        result = self.run_app(candidate, "открой входную дверь\nоткрой дверь\n")
        self.assert_ready(result, ["D", "U"])
        self.assertNotIn("legacy_shadow", result.stderr)

    def test_enablement_and_deny_are_not_an_additional_cutover(self):
        candidate = copy.deepcopy(SAMPLE)
        self.door(candidate)["enabled"] = False
        next(action for action in candidate["actions"]
             if action["id"] == candidate["compatibility"]["door_action_id"])["enabled"] = False
        candidate["safety"]["restrictions"] = [{"id": "deny_entrance", "name": "Запрет (тест)",
                                                "kind": "deny_action", "device_id": "device_entrance_door",
                                                "action_id": "action_open_entrance"}]
        self.assert_ready(self.run_app(candidate, "открой входную дверь\nоткрой дверь\n"), ["D", "U"])


if __name__ == "__main__":
    unittest.main()
