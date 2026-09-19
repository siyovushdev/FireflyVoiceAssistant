"""Real text entry point with the unchanged fake RKLLM; no models/NPU."""
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
SAMPLE = json.loads((ROOT / "config/current-m4.json").read_text())
sys.argv = [sys.argv[0]]


class RuntimeConfigCliTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="firefly-runtime-cli-")
        self.addCleanup(self.scratch.cleanup)
        self.directory = Path(self.scratch.name)
        self.config = self.directory / "current.json"
        self.config.write_text(json.dumps(SAMPLE, ensure_ascii=False))
        self.prompt = self.directory / "prompt.txt"
        self.prompt.write_text("frozen")
        self.args = [str(APP), "--model", "fake", "--system-prompt", str(self.prompt)]

    def run_app(self, *extra, text="на кухне свет 40\n"):
        return subprocess.run(self.args + list(map(str, extra)), input=text, text=True,
                              capture_output=True, timeout=15, cwd=self.directory)

    def assert_startup_failure(self, result):
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(result.stdout, "U\n")
        for stage in ("READY", "CONFIG_RUNTIME", "stub destroyed", "Loading RKLLM", "Loading model", "Building system KV"):
            self.assertNotIn(stage, result.stderr)
        self.assertIn("Error:", result.stderr)

    def test_default_path_is_independent_of_cwd(self):
        result = self.run_app()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "L|K|40\n")
        self.assertEqual(result.stderr.count("CONFIG_RUNTIME"), 1)
        self.assertIn("action_model=generic domain_authority=config", result.stderr)
        self.assertLess(result.stderr.index("CONFIG_RUNTIME"), result.stderr.index("READY"))

    def test_complete_current_output_domain(self):
        locations = {"K": "на кухне", "B": "в спальне", "G": "в гостиной", "C": "в коридоре"}
        commands, expected = [], []
        for code, location in locations.items():
            for value in range(101):
                commands.append(f"{location} свет {value}")
                expected.append(f"L|{code}|{value}")
        commands += ["Открой входную дверь", "открытвер на входе", "ванная свет 40"]
        expected += ["D", "U", "U"]
        result = self.run_app("--config", self.config, text="\n".join(commands) + "\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), expected)
        self.assertIn("action_model=generic domain_authority=config", result.stderr)

    def test_invalid_files_fail_before_model_start(self):
        variants = ["{", "{}", '{"schema_version":1,"schema_version":1}']
        for change in ("version", "range", "safety", "missing", "reference", "alias"):
            candidate = copy.deepcopy(SAMPLE)
            if change == "version":
                candidate["schema_version"] = 999
            elif change == "range":
                candidate["actions"][0]["parameters"][0]["maximum"] = 101
            elif change == "safety":
                candidate["safety"]["skip_safety"] = True
            elif change == "missing":
                del candidate["rooms"]
            elif change == "reference":
                candidate["devices"][0]["room_id"] = "missing_room"
            elif change == "alias":
                candidate["rooms"][1]["speech"]["mentions"].append("кухня")
            variants.append(json.dumps(candidate))
        for variant in variants:
            with self.subTest(variant=variant[:100]):
                self.config.write_text(variant)
                self.assert_startup_failure(self.run_app("--config", self.config))

    def test_missing_and_symlink_do_not_fall_back(self):
        self.assert_startup_failure(self.run_app("--config", self.directory / "missing.json"))
        link = self.directory / "link.json"
        link.symlink_to(self.config)
        self.assert_startup_failure(self.run_app("--config", link))
        self.assert_startup_failure(self.run_app("--config", self.directory))

    def test_config_option_is_unambiguous(self):
        for extra in (["--config"], ["--config", ""],
                      ["--config", self.config, "--config", self.config]):
            self.assert_startup_failure(self.run_app(*extra))

    def test_valid_drift_changes_only_migrated_brightness_support(self):
        candidate = copy.deepcopy(SAMPLE)
        candidate["actions"][0]["parameters"][0]["maximum"] = 99
        candidate["devices"][-1]["enabled"] = False
        self.config.write_text(json.dumps(candidate))
        commands = "на кухне свет 100\nОткрой входную дверь\nоткрытвер на входе\n"
        result = self.run_app("--config", self.config, text=commands)
        repeated = self.run_app("--config", self.config, text=commands)
        self.assertEqual(result.returncode, 0, result.stderr)
        # 6.5 activates the narrower range; enablement policy is still inactive.
        self.assertEqual(result.stdout, "U\nD\nU\n")
        diagnostic = next(line for line in result.stderr.splitlines() if line.startswith("CONFIG_RUNTIME"))
        self.assertIn("action_model=generic domain_authority=config", diagnostic)
        self.assertNotIn("legacy_shadow", diagnostic)
        self.assertEqual(diagnostic, next(line for line in repeated.stderr.splitlines() if line.startswith("CONFIG_RUNTIME")))

    def test_snapshot_is_held_after_source_file_becomes_invalid(self):
        # Unbuffered bytes allow select() to observe each startup line reliably.
        process = subprocess.Popen(self.args + ["--config", str(self.config)],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
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
            self.config.write_text("{")
            stdout, stderr = process.communicate("на кухне свет 40\nОткрой входную дверь\n".encode(), timeout=15)
            self.assertEqual(process.returncode, 0, stderr)
            self.assertEqual(stdout, b"L|K|40\nD\n")
            self.assertEqual((startup + stderr).count(b"CONFIG_RUNTIME"), 1)
            self.assertIn(b"action_model=generic domain_authority=config", startup)
            self.assert_startup_failure(self.run_app("--config", self.config))
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate()


if __name__ == "__main__":
    unittest.main()
