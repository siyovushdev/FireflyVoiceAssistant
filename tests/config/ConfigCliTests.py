"""Offline CLI, artifact, and source-domain oracles. No models or audio used."""
import copy
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

APP = Path(sys.argv[1]).resolve()
ROOT = Path(sys.argv[2]).resolve()
SAMPLE = ROOT / "config/current-m4.json"
sys.argv = [sys.argv[0]]


class ConfigCliTests(unittest.TestCase):
    def run_tool(self, *args, valid=True):
        result = subprocess.run([str(APP), *map(str, args)], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode == 0, valid, result.stdout + result.stderr)
        if valid:
            self.assertEqual(result.stderr, "")
            data = json.loads(result.stdout)
        else:
            self.assertEqual(result.stdout, "")
            data = json.loads(result.stderr)
        self.assertFalse(data["production_activated"])
        return data

    def test_golden_summary(self):
        data = self.run_tool("inspect", "--file", SAMPLE)
        golden = json.loads((ROOT / "tests/config/current-summary.json").read_text())
        self.assertEqual(data, golden)
        self.assertEqual(data, self.run_tool("validate", "--file", SAMPLE))

    def test_source_domain_shadow(self):
        config = json.loads(SAMPLE.read_text())
        rooms = {room["id"]: room for room in config["rooms"]}
        enum_ids = {"Kitchen": "room_kitchen", "Bedroom": "room_bedroom", "LivingRoom": "room_living", "Corridor": "room_corridor"}
        action_header = (ROOT / "include/firefly/Action.h").read_text()
        self.assertEqual(set(re.search(r"enum class Room \{([^}]+)\}", action_header)[1].replace(" ", "").split(",")), set(enum_ids))
        gate = (ROOT / "src/SafetyGate.cpp").read_text()
        found = dict(re.findall(r"case Room::(\w+): rooms = \{([^}]+)\}", gate))
        self.assertEqual(set(found), set(enum_ids))
        for enum, room_id in enum_ids.items():
            self.assertEqual(re.findall(r'"([^"]+)"', found[enum]), rooms[room_id]["speech"]["mentions"])
        parser = (ROOT / "src/DeterministicParser.cpp").read_text()
        locations = re.findall(r'\{"([^"]+)", \'([KBGC])\'\}', parser)
        devices = {device["id"]: device for device in config["devices"]}
        expected = {}
        for phrase, code in locations:
            target = devices[config["compatibility"]["light_targets"][code]]
            expected.setdefault(target["room_id"], []).append(phrase)
        self.assertEqual(expected, {key: room["speech"]["locations"] for key, room in rooms.items()})
        self.assertEqual(config["bindings"], [])
        report = (ROOT / "docs/milestone-6.1-dynamic-configuration-design.md").read_text()
        self.assertEqual(config, json.loads(re.search(r"```json\n(.*?)\n```", report, re.S)[1]))

    def test_closed_schema_artifact(self):
        schema = json.loads((ROOT / "config/schema-v1.json").read_text())
        self.assertEqual(schema["$schema"], "https://json-schema.org/draft/2020-12/schema")
        self.assertEqual(schema["properties"]["schema_version"]["const"], 1)
        supported = {"$schema", "$id", "title", "$defs", "$ref", "type", "properties", "required", "additionalProperties", "items", "minItems", "maxItems", "minLength", "maxLength", "pattern", "minimum", "maximum", "const", "enum"}

        def visit(node):
            self.assertTrue(set(node) <= supported)
            if node.get("type") == "object":
                self.assertFalse(node["additionalProperties"])
                self.assertEqual(set(node["required"]), set(node["properties"]))
            if "$ref" in node:
                self.assertTrue(node["$ref"].startswith("#/$defs/"))
                self.assertIn(node["$ref"].split("/")[-1], schema["$defs"])
            for group in ("$defs", "properties"):
                for child in node.get(group, {}).values():
                    visit(child)
            if "items" in node:
                visit(node["items"])
        visit(schema)

    def test_invalid_cli(self):
        for args in [[], ["wat"], ["validate"], ["validate", "--file", SAMPLE, "--file", SAMPLE], ["inspect", "--file", SAMPLE, "--activate", "yes"]]:
            with self.subTest(args=args):
                self.assertEqual(self.run_tool(*args, valid=False)["code"], "cli_usage")

    def test_no_partial_result(self):
        with tempfile.TemporaryDirectory(prefix="firefly-config-cli-") as directory:
            path = Path(directory) / "invalid.json"
            data = json.loads(SAMPLE.read_text())
            data["devices"][-1]["action_ids"] = ["missing"]
            path.write_text(json.dumps(data))
            self.assertEqual(self.run_tool("inspect", "--file", path, valid=False)["code"], "dangling_reference")

    def test_store_cli(self):
        with tempfile.TemporaryDirectory(prefix="firefly-config-cli-") as directory:
            first = self.run_tool("init", "--store", directory, "--file", SAMPLE)
            committed = Path(directory) / "configuration.json"
            self.assertEqual(hashlib.sha256(committed.read_bytes()).hexdigest(), first["content_sha256"])
            self.assertEqual(committed.stat().st_mode & 0o777, 0o600)
            change = copy.deepcopy(json.loads(SAMPLE.read_text()))
            change["system"]["name"] = "Переименованный дом"
            candidate = Path(directory) / "candidate.json"
            candidate.write_text(json.dumps(change, ensure_ascii=False))
            second = self.run_tool("save", "--store", directory, "--file", candidate, "--expected-revision", 1, "--expected-hash", first["content_sha256"])
            self.assertEqual(second["revision"], 2)
            self.assertEqual(self.run_tool("save", "--store", directory, "--file", candidate, "--expected-revision", 1, "--expected-hash", first["content_sha256"], valid=False)["code"], "revision_conflict")
            revisions = self.run_tool("revisions", "--store", directory)["revisions"]
            self.assertEqual(revisions, [{"revision": 1, "content_sha256": first["content_sha256"]}])
            third = self.run_tool("restore", "--store", directory, "--revision", 1, "--expected-revision", 2, "--expected-hash", second["content_sha256"])
            self.assertEqual(third["revision"], 3)
            self.assertEqual(third, self.run_tool("current", "--store", directory))
            self.assertEqual(third, self.run_tool("reconcile", "--store", directory))


if __name__ == "__main__":
    unittest.main()
