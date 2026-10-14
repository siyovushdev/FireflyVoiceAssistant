"""Relocatable packaging contract. Uses local libraries and fixture models.

No NPU, microphone, production service, /opt install, or model download.
"""
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from packaging import firefly_release as release  # noqa: E402
from packaging.firefly_release import (  # noqa: E402
    ADMIN_MODEL, AMINA_SHA256, LIVE_STORE, PRODUCTION_PREFIX, PRODUCTION_SHA256,
    RUNTIME_MODEL, STT_DIR, STT_FILES, VAD_MODEL, ReleaseError, StageRequest,
    activate, assess_post_start, assert_activation_destination, assert_schema_compatible,
    assert_stage_destination, classify_systemd_analyze, install_release,
    install_systemd_unit, read_ready_byte, render_unit, require_store, restore_systemd_unit,
    rollback, stage_release, unit_paths_exist, verify_tree, _stop_process,
)


def parse_args(argv):
    values = {}
    tokens = iter(argv)
    for token in tokens:
        if not token.startswith("--"):
            raise SystemExit("unexpected argument " + token)
        values[token[2:]] = next(tokens)
    return values


ARGS = parse_args(sys.argv[1:])
# unittest must not see the packaging arguments.
sys.argv = [sys.argv[0]]


def request(destination, release_id, **overrides):
    models = Path(destination) / "inputs"
    base = StageRequest(
        destination=Path(destination) / "prefix",
        build_dir=Path(ARGS["build"]),
        source_root=Path(ARGS["source"]),
        rkllm_library=Path(ARGS["rkllm-library"]),
        sherpa_library=Path(ARGS["sherpa-library"]),
        onnx_library=Path(ARGS["onnx-library"]),
        runtime_model=models / "runtime.rkllm",
        admin_model=models / "admin.rkllm",
        stt_dir=models / "stt",
        vad_model=models / "vad.onnx",
        wake_site_packages=Path(ARGS["wake-site"]),
        release_id=release_id,
        commit="",
        runtime_prefix="",
        production=False,
    )
    for key, value in overrides.items():
        setattr(base, key, value)
    return base


def write_fixtures(root):
    models = root / "inputs"
    (models / "stt").mkdir(parents=True)
    (models / "runtime.rkllm").write_bytes(b"runtime-fixture")
    (models / "admin.rkllm").write_bytes(b"admin-fixture")
    (models / "vad.onnx").write_bytes(b"vad-fixture")
    for name in STT_FILES:
        (models / "stt" / name).write_bytes(name.encode())


def _ready_status():
    names = ("admin", "configuration", "runtime", "llm", "stt", "wake")
    return {
        "ok": True,
        "runtime_ready": True,
        "stopping": False,
        "stt_ready": True,
        "microphone": "connected",
        "voice": "WAITING_WAKE",
        "health": {
            "overall": "ready",
            "stopping": False,
            "subsystems": {name: {"status": "ready"} for name in names},
        },
    }


def secure_store(path, text):
    path.mkdir(exist_ok=True)
    path.chmod(0o700)
    config = path / "configuration.json"
    config.write_text(text)
    config.chmod(0o600)
    return config


def canonical_text():
    return (Path(ARGS["source"]) / "config/empty-home.json").read_text()


def snapshot(path):
    if not path.exists():
        return None
    return {
        str(item.relative_to(path)): hashlib.sha256(item.read_bytes()).hexdigest()
        for item in path.rglob("*") if item.is_file()
    }


def matching_clean_build_identity(source):
    """Test input for isolating provenance failures from an older local build."""
    return {
        **release.build_identity(Path(ARGS["build"]) / "firefly-demo"),
        "head": source["commit"],
        "dirty": False,
        "source_sha256": release.source_tree_fingerprint(Path(ARGS["source"])),
        "admin_assets_sha256": release.admin_assets_fingerprint(Path(ARGS["build"])),
    }


class ProductionPackaging(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.previous_umask = os.umask(0o022)
        if not Path(ARGS.get("wake-site", "")).is_dir():
            raise AssertionError("FIREFLY_WAKE_PYTHON site-packages are required for packaging tests")
        cls.temp = tempfile.TemporaryDirectory(prefix="firefly-package-")
        cls.root = Path(cls.temp.name)
        write_fixtures(cls.root)
        cls.prefix = cls.root / "prefix"
        cls.release_id = "m18.6-fixture-a"
        cls.manifest = stage_release(request(cls.root, cls.release_id))
        cls.release = cls.prefix / "releases" / cls.release_id
        cls.before = snapshot(cls.release)
        if (cls.prefix / "current").exists():
            raise AssertionError("staging created current")

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()
        os.umask(cls.previous_umask)

    def test_installed_models_stage_install_failure_cleanup_and_legacy_verify(self):
        if not re.search(r'^FIREFLY_WAKE_MODEL_REFERENCE:FILEPATH=.+$',
                         (Path(ARGS['build'])/'CMakeCache.txt').read_text(), re.M):
            self.skipTest('requires explicitly reference-configured build; mandatory for no-copy candidate')
        with tempfile.TemporaryDirectory(prefix="firefly-reference-installed-") as private:
            installed = Path(private) / "installed"
            legacy = installed / "releases" / self.release_id
            shutil.copytree(self.release, legacy)
            for name in release.ALLOWED_SHARED_MODELS:
                release._copy_regular(self.prefix/name, installed/name)
            inputs={name:installed/name for name in release.ALLOWED_SHARED_MODELS}
            frozen={**PRODUCTION_SHA256, **{name:release.sha256_file(path) for name,path in inputs.items()}}
            protected = {str(p):p.stat() for p in [*inputs.values(),legacy/release.WAKE_MODEL]}
            current = installed/'current';current.symlink_to('releases/'+self.release_id)
            marker=installed/'previous-release';marker.write_text(self.release_id+'\n')
            new_id='installed-reference-e2e'
            with tempfile.TemporaryDirectory(prefix='firefly-reference-') as tmp, \
                 patch.object(release,'PRODUCTION_PREFIX',str(installed)), \
                 patch.object(release,'PRODUCTION_SHA256',frozen), \
                 patch.object(release,'ADMIN_MODEL_BYTES',inputs[ADMIN_MODEL].stat().st_size), \
                 patch.object(release,'require_root_owned'), \
                 patch.object(release,'assert_production_ownership'), \
                 patch.dict(os.environ, {'FIREFLY_OWNER_ACCEPTED':'1'}):
                req=request(Path(tmp),new_id,production=True,runtime_prefix=str(installed),
                            runtime_model=inputs[RUNTIME_MODEL],admin_model=inputs[ADMIN_MODEL],
                            vad_model=inputs[VAD_MODEL],stt_dir=installed/STT_DIR,
                            reuse_installed_models=True,wake_reference_release=self.release_id)
                with patch.object(release,'_copy_regular',wraps=release._copy_regular) as copies:
                    document=stage_release(req)
                    self.assertFalse((req.destination/'models').exists())
                    self.assertTrue((req.destination/'releases'/new_id/release.WAKE_MODEL).is_symlink())
                    self.assertFalse(any(call.args[0] in inputs.values() for call in copies.call_args_list))
                    # A failure after publication cleans only the new app release, never shared models.
                    with patch.object(release,'install_after_models',side_effect=RuntimeError('fixture fault')):
                        with self.assertRaises(ReleaseError): install_release(req.destination,installed,new_id)
                    self.assertFalse((installed/'releases'/new_id).exists())
                    install_release(req.destination,installed,new_id)
                    self.assertFalse(any(call.args[0] in inputs.values() for call in copies.call_args_list))
                verify_tree(installed,new_id,check_python=True)
                self.assertEqual(os.readlink(current),'releases/'+self.release_id)
                self.assertEqual(marker.read_text(),self.release_id+'\n')
                self.assertEqual(document['installed_model_reference']['mode'],'installed-read-only')
                for name,before in protected.items():
                    after=Path(name).stat()
                    self.assertEqual((after.st_ino,after.st_nlink,after.st_mode,after.st_mtime_ns,after.st_ctime_ns),
                                     (before.st_ino,before.st_nlink,before.st_mode,before.st_mtime_ns,before.st_ctime_ns))
                shutil.rmtree(installed/'releases'/new_id)
            verify_tree(installed,self.release_id,check_python=True)
            current.unlink();marker.unlink()

    def test_manifest_layout_and_shared_models(self):
        self.assertEqual(self.manifest["release_id"], self.release_id)
        self.assertEqual(self.manifest["architecture"], "aarch64")
        self.assertEqual(self.manifest["source_commit"], subprocess.check_output(
            ["git", "-C", ARGS["source"], "rev-parse", "HEAD"], text=True).strip())
        source = release.source_identity(Path(ARGS["source"]))
        self.assertEqual(self.manifest["source"]["dirty"], source["dirty"])
        self.assertEqual(self.manifest["source"]["identity"],
                         "dirty-worktree" if source["dirty"] else "committed-fixture")
        self.assertFalse(self.manifest["production_identity_checked"])
        self.assertEqual(self.manifest["config_schema"]["readable"], [1, 2])
        self.assertFalse(self.manifest["systemd"]["installed"])
        self.assertFalse(self.manifest["activation"]["creates_current_symlink"])
        self.assertIn("lib/librkllmrt.so", self.manifest["distribution"]["same_device_only"])
        for relative in ("bin/firefly-demo", "lib/librkllmrt.so", "lib/libsherpa-onnx-c-api.so",
                         "lib/libonnxruntime.so", "share/firefly/config/empty-home.json",
                         "share/firefly/wake/amina-v3.tflite", "share/firefly/wake/amina_worker.py",
                         "share/firefly/wake/python/bin/python"):
            self.assertTrue((self.release / relative).is_file(), relative)
        for relative in (RUNTIME_MODEL, ADMIN_MODEL, VAD_MODEL, *(f"{STT_DIR}/{name}" for name in STT_FILES)):
            self.assertTrue((self.prefix / relative).is_file(), relative)
        self.assertEqual(hashlib.sha256((self.release / "share/firefly/wake/amina-v3.tflite").read_bytes()).hexdigest(), AMINA_SHA256)
        self.assertEqual(
            hashlib.sha256((self.release / "lib/librkllmrt.so").read_bytes()).hexdigest(),
            PRODUCTION_SHA256["lib/librkllmrt.so"])
        self.assertNotIn("configuration.json", [item["path"] for item in self.manifest["artifacts"]])
        verify_tree(self.prefix, self.release_id, check_python=False)

    def test_clean_and_dirty_fixture_identities(self):
        source = release.source_identity(Path(ARGS["source"]))
        for dirty in (False, True):
            with self.subTest(dirty=dirty):
                identity = {**source, "dirty": dirty,
                            "identity": "dirty-worktree" if dirty else "committed"}
                release_id = "m18.6-dirty-fixture" if dirty else "m18.6-clean-fixture"
                with patch.object(release, "source_identity", return_value=identity):
                    manifest = stage_release(request(self.root, release_id))
                self.assertEqual(manifest["source_commit"], source["commit"])
                self.assertEqual(manifest["source"]["dirty"], dirty)
                self.assertEqual(manifest["source"]["identity"],
                                 "dirty-worktree" if dirty else "committed-fixture")
                self.assertFalse(manifest["production_identity_checked"])
                if dirty:
                    self.assertFalse(manifest["build"]["matches_worktree"])
                verify_tree(self.prefix, release_id, check_python=False)

    def test_runtime_paths_do_not_use_the_checkout(self):
        source = str(Path(ARGS["source"]).resolve())
        for relative in ("manifest.json", "share/firefly/systemd/firefly-voice-assistant.service",
                         "share/firefly/wake/python/bin/python"):
            text = (self.release / relative).read_text()
            self.assertNotIn(source, text, relative)
            self.assertNotIn("/home/firefly", text, relative)
            self.assertNotIn("demo_Linux_aarch64", text, relative)
        dynamic = subprocess.check_output(["readelf", "-d", str(self.release / "bin/firefly-demo")], text=True)
        self.assertIn("$ORIGIN/../lib", dynamic)
        self.assertNotIn("/home/firefly", dynamic)
        env = {key: value for key, value in os.environ.items() if key != "LD_LIBRARY_PATH"}
        listing = subprocess.check_output(["ldd", str(self.release / "bin/firefly-demo")], env=env, text=True)
        self.assertNotIn("not found", listing)
        self.assertNotIn(source, listing)
        lib_dir = (self.release / "lib").resolve()
        for name in ("librkllmrt.so", "libsherpa-onnx-c-api.so", "libonnxruntime.so"):
            match = re.search(rf"{re.escape(name)} => (\S+)", listing)
            self.assertIsNotNone(match, name)
            self.assertEqual(Path(match.group(1)).resolve().parent, lib_dir)

    def test_activation_unit_paths_and_rollback_preserve_store(self):
        clone = self.prefix / "releases" / "m18.6-fixture-b"
        shutil.copytree(self.release, clone)
        document = json.loads((clone / "manifest.json").read_text())
        document["release_id"] = "m18.6-fixture-b"
        text = json.dumps(document, indent=2, sort_keys=True) + "\n"
        (clone / "manifest.json").write_text(text)
        (clone / "manifest.sha256").write_text(hashlib.sha256(text.encode()).hexdigest() + "\n")
        store = self.root / "store"
        config = secure_store(store, canonical_text())
        before = config.read_bytes()
        other = store / "last_known_good.json"
        other.write_text('{"revision": 7}\n')
        other_before = other.read_bytes()
        message = activate(self.prefix, self.release_id, store, config_tool=Path(ARGS["config-tool"]))
        self.assertIn("service restart was not run", message)
        self.assertIn("ConfigStore was not modified", message)
        self.assertEqual(os.readlink(self.prefix / "current"), "releases/" + self.release_id)
        unit_paths_exist(self.prefix, self.release_id)
        activate(self.prefix, "m18.6-fixture-b", store, config_tool=Path(ARGS["config-tool"]))
        self.assertEqual((self.prefix / "previous-release").read_text().strip(), self.release_id)
        self.assertEqual(config.read_bytes(), before)
        rolled = rollback(self.prefix, store, config_tool=Path(ARGS["config-tool"]))
        self.assertIn("rolled back to " + self.release_id, rolled)
        self.assertEqual(os.readlink(self.prefix / "current"), "releases/" + self.release_id)
        self.assertEqual(config.read_bytes(), before)
        self.assertEqual(other.read_bytes(), other_before)
        config.write_text('{"schema_version": 99, "revision": 7}\n')
        config.chmod(0o600)
        with self.assertRaises(ReleaseError):
            rollback(self.prefix, store, config_tool=Path(ARGS["config-tool"]))
        self.assertEqual(os.readlink(self.prefix / "current"), "releases/" + self.release_id)
        self.assertEqual(config.read_text(), '{"schema_version": 99, "revision": 7}\n')
        config.write_text('{"schema_version": 2}\n')
        config.chmod(0o600)
        with self.assertRaises(ReleaseError):
            rollback(self.prefix, store, config_tool=Path(ARGS["config-tool"]))
        self.assertEqual(os.readlink(self.prefix / "current"), "releases/" + self.release_id)
        self.assertEqual(config.read_text(), '{"schema_version": 2}\n')
        config.write_text(before.decode())
        config.chmod(0o600)
        with self.assertRaises(ReleaseError):
            assert_schema_compatible(3, [1, 2])

    def test_missing_and_corrupt_artifacts_publish_nothing(self):
        raw = request(self.root, "m18.6-missing")
        raw.vad_model = self.root / "inputs" / "missing-vad.onnx"
        with self.assertRaises(ReleaseError):
            stage_release(raw)
        self.assertFalse((self.prefix / "releases" / "m18.6-missing").exists())
        self.assertEqual(snapshot(self.release), self.before)
        empty = self.root / "empty-prefix"
        bad = request(self.root, "m18.6-corrupt", production=True)
        bad.destination = empty
        with self.assertRaises(ReleaseError):
            stage_release(bad)
        self.assertFalse(empty.exists())

    def test_failed_stage_does_not_mutate_an_installed_release(self):
        original = __import__("packaging.firefly_release", fromlist=["check_wake"])
        real = original.check_wake

        def boom(*_args, **_kwargs):
            raise ReleaseError("injected wake failure")

        original.check_wake = boom
        try:
            with self.assertRaises(ReleaseError):
                stage_release(request(self.root, "m18.6-boom"))
        finally:
            original.check_wake = real
        self.assertFalse((self.prefix / "releases" / "m18.6-boom").exists())
        self.assertEqual(snapshot(self.release), self.before)
        self.assertFalse(any(path.name.startswith(".staging-") for path in self.prefix.iterdir()))
        fresh = self.root / "second"
        write_fixtures(fresh)
        (fresh / "inputs" / "runtime.rkllm").write_bytes(b"different-runtime")
        staged = request(fresh, "m18.6-other")
        staged.destination = self.prefix
        staged.runtime_model = fresh / "inputs" / "runtime.rkllm"
        with self.assertRaises(ReleaseError):
            stage_release(staged)
        self.assertEqual((self.prefix / RUNTIME_MODEL).read_bytes(), b"runtime-fixture")
        self.assertEqual(snapshot(self.release), self.before)
        self.assertFalse((self.prefix / "releases" / "m18.6-other").exists())

    def test_live_roots_and_health_gate(self):
        with self.assertRaises(ReleaseError):
            assert_stage_destination(Path("/opt/firefly/voice-assistant"), Path(ARGS["source"]))
        with self.assertRaises(ReleaseError):
            assert_stage_destination(LIVE_STORE, Path(ARGS["source"]))
        with self.assertRaises(ReleaseError):
            assert_stage_destination(Path("/home/firefly/FireflyVoiceAssistantLinux/build"), Path(ARGS["source"]))
        with self.assertRaises(ReleaseError):
            assert_activation_destination(Path("/opt/firefly/voice-assistant"))
        with self.assertRaises(ReleaseError):
            require_store(LIVE_STORE, [1, 2], config_tool=Path(ARGS["config-tool"]))
        ready = _ready_status()
        self.assertEqual(assess_post_start(ready, "http://127.0.0.1:8080/api/status"), [])
        self.assertTrue(assess_post_start(ready, "http://0.0.0.0:8080/api/status"))
        for mutant in (
            {**ready, "runtime_ready": False},
            {**ready, "stt_ready": False},
            {**ready, "microphone": "unavailable"},
            {**ready, "voice": "listening"},
            {**ready, "health": {**ready["health"], "overall": "degraded"}},
            {**ready, "health": {**ready["health"], "subsystems": {**ready["health"]["subsystems"], "wake": {"status": "failed"}}}},
            {**ready, "health": {**ready["health"], "subsystems": {**ready["health"]["subsystems"], "llm": {"status": "starting"}}}},
            {**ready, "health": {**ready["health"], "subsystems": {**ready["health"]["subsystems"], "stt": {"status": "unavailable"}}}},
        ):
            self.assertTrue(assess_post_start(mutant, "http://127.0.0.1:8080/api/status"))
        thin = {"ok": True, "stopping": False, "health": {"overall": "ready", "stopping": False, "subsystems": {
            "admin": {"status": "ready"}, "configuration": {"status": "ready"}, "runtime": {"status": "ready"}}}}
        self.assertTrue(assess_post_start(thin, "http://127.0.0.1:8080/api/status"))
        checked = (Path(ARGS["source"]) / "packaging/firefly-voice-assistant.service").read_text()
        self.assertEqual(checked, render_unit(PRODUCTION_PREFIX))
        classified = classify_systemd_analyze(
            "netplan-ovs-cleanup.service: Failed to open /run/systemd/system/netplan-ovs-cleanup.service: Permission denied\n"
            "Configuration file /lib/systemd/system/rkaiq_3A.service is marked world-writable.\n"
            "firefly-voice-assistant.service: Command /opt/firefly/voice-assistant/current/bin/firefly-demo "
            "is not executable: No such file or directory\n"
            "firefly-voice-assistant.service: Unknown key name 'Broken' in section 'Service'\n")
        self.assertEqual(len(classified["expected_absent_install"]), 1)
        self.assertEqual(classified["defects"], ["firefly-voice-assistant.service: Unknown key name 'Broken' in section 'Service'"])
        self.assertEqual(len(classified["foreign"]), 2)

    def test_cli_render_and_health(self):
        script = Path(ARGS["source"]) / "scripts/stage-release.py"
        rendered = subprocess.check_output([sys.executable, str(script), "render-unit"], text=True)
        self.assertEqual(rendered, render_unit(PRODUCTION_PREFIX))
        status = self.root / "status.json"
        status.write_text(json.dumps(_ready_status()))
        bad = subprocess.run([sys.executable, str(script), "health-gate", "--url", "http://10.0.0.8:8080/api/status",
                              "--status-file", str(status)], text=True, capture_output=True)
        self.assertEqual(bad.returncode, 1)
        self.assertIn("127.0.0.1", bad.stderr)
        good = subprocess.run([sys.executable, str(script), "health-gate", "--url", "http://127.0.0.1:8080/api/status",
                               "--status-file", str(status)], text=True, capture_output=True)
        self.assertEqual(good.returncode, 0, good.stderr)

    def test_store_must_be_compatible_before_switch(self):
        current = os.readlink(self.prefix / "current") if (self.prefix / "current").is_symlink() else None
        missing = self.root / "missing-store"
        missing.mkdir()
        with self.assertRaises(ReleaseError):
            activate(self.prefix, self.release_id, missing, config_tool=Path(ARGS["config-tool"]))
        corrupt = self.root / "corrupt-store"
        corrupt.mkdir()
        (corrupt / "configuration.json").write_text("{")
        with self.assertRaises(ReleaseError):
            activate(self.prefix, self.release_id, corrupt, config_tool=Path(ARGS["config-tool"]))
        blocked = self.root / "blocked-store"
        blocked.mkdir()
        config = blocked / "configuration.json"
        config.write_text('{"schema_version": 2}\n')
        config.chmod(0)
        try:
            with self.assertRaises(ReleaseError):
                activate(self.prefix, self.release_id, blocked, config_tool=Path(ARGS["config-tool"]))
        finally:
            config.chmod(0o600)
        incompatible = self.root / "schema-store"
        incompatible.mkdir()
        (incompatible / "configuration.json").write_text('{"schema_version": 99}\n')
        with self.assertRaises(ReleaseError):
            activate(self.prefix, self.release_id, incompatible, config_tool=Path(ARGS["config-tool"]))
        with self.assertRaises(ReleaseError):
            rollback(self.prefix, incompatible, config_tool=Path(ARGS["config-tool"]))
        after = os.readlink(self.prefix / "current") if (self.prefix / "current").is_symlink() else None
        self.assertEqual(after, current)

    def test_fresh_initialization_does_not_write_the_store(self):
        store = self.root / "fresh-store"
        store.mkdir()
        before = os.readlink(self.prefix / "current")
        store.chmod(0o700)
        activate(self.prefix, self.release_id, store, fresh=True, config_tool=Path(ARGS["config-tool"]))
        self.assertFalse((store / "configuration.json").exists())
        self.assertEqual(sorted(path.name for path in store.iterdir()), [])
        occupied = self.root / "occupied-store"
        occupied.mkdir()
        (occupied / "configuration.json").write_text('{"schema_version": 2}\n')
        with self.assertRaises(ReleaseError):
            activate(self.prefix, self.release_id, occupied, fresh=True, config_tool=Path(ARGS["config-tool"]))
        self.assertEqual((occupied / "configuration.json").read_text(), '{"schema_version": 2}\n')
        # Put current back only if this call changed it; the bytes of the store stay empty.
        if os.readlink(self.prefix / "current") != before and (self.prefix / "previous-release").is_file():
            rollback(self.prefix, self.root / "store", config_tool=Path(ARGS["config-tool"]))

    def test_publish_failure_removes_only_new_models(self):
        work = self.root / "publish-work"
        dest = self.root / "publish-dest"
        (work / "models/runtime").mkdir(parents=True)
        (work / "models/admin").mkdir(parents=True)
        (work / RUNTIME_MODEL).write_bytes(b"new-runtime")
        (work / ADMIN_MODEL).write_bytes(b"new-admin")
        (dest / "models/vad").mkdir(parents=True)
        (dest / VAD_MODEL).write_bytes(b"keep-vad")
        (dest / "releases/keep").mkdir(parents=True)
        (dest / "releases/keep/marker").write_text("same")
        (dest / "current").symlink_to("releases/keep")
        real_link = os.link

        def link(source, target, *args, **kwargs):
            if str(target).endswith("Qwen3-4B_W8A8_CTX4096_RK3588_3core.rkllm"):
                Path(target).parent.mkdir(parents=True, exist_ok=True)
                Path(target).write_bytes(b"preexisting-admin")
                raise FileExistsError(target)
            return real_link(source, target, *args, **kwargs)

        os.link = link
        try:
            with self.assertRaises(ReleaseError):
                release.publish_shared_models(work, dest)
        finally:
            os.link = real_link
        self.assertFalse((dest / RUNTIME_MODEL).exists())
        self.assertEqual((dest / ADMIN_MODEL).read_bytes(), b"preexisting-admin")
        self.assertEqual((dest / VAD_MODEL).read_bytes(), b"keep-vad")
        self.assertEqual((dest / "releases/keep/marker").read_text(), "same")
        self.assertEqual(os.readlink(dest / "current"), "releases/keep")
        self.assertFalse(any(path.name.startswith(".") and "incoming" in path.name for path in dest.rglob("*")))

        def boom(relative):
            if relative == ADMIN_MODEL:
                raise ReleaseError("injected publication failure")

        (work / "models/runtime").mkdir(parents=True, exist_ok=True)
        (work / RUNTIME_MODEL).write_bytes(b"new-runtime")
        (work / ADMIN_MODEL).write_bytes(b"new-admin")
        (dest / ADMIN_MODEL).unlink()
        release.publish_after = boom
        try:
            with self.assertRaises(ReleaseError):
                release.publish_shared_models(work, dest)
        finally:
            release.publish_after = None
        self.assertFalse((dest / RUNTIME_MODEL).exists())
        self.assertFalse((dest / ADMIN_MODEL).exists())
        self.assertEqual((dest / VAD_MODEL).read_bytes(), b"keep-vad")
        self.assertEqual(os.readlink(dest / "current"), "releases/keep")

    def test_wake_ready_deadline_reaps_a_silent_worker(self):
        process = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(30)"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            with self.assertRaises(ReleaseError) as caught:
                read_ready_byte(process, 0.4)
            self.assertIn("ready byte", str(caught.exception))
        finally:
            _stop_process(process)
        self.assertIsNotNone(process.poll())

    def test_disk_preflight_and_model_reuse(self):
        real = release.shutil.disk_usage
        release.shutil.disk_usage = lambda _path: type("Usage", (), {"free": 1, "total": 2, "used": 1})()
        try:
            with self.assertRaises(ReleaseError) as caught:
                stage_release(request(self.root, "m18.6-nospace"))
        finally:
            release.shutil.disk_usage = real
        self.assertIn("bytes free", str(caught.exception))
        self.assertFalse((self.prefix / "releases" / "m18.6-nospace").exists())
        self.assertEqual(snapshot(self.release), self.before)
        release.copied_destinations.clear()
        stage_release(request(self.root, "m18.6-reuse"))
        copied = "\n".join(release.copied_destinations)
        self.assertNotIn("Qwen3-4B_W8A8_RK3588_3core.rkllm", copied)
        self.assertNotIn("Qwen3-4B_W8A8_CTX4096_RK3588_3core.rkllm", copied)
        self.assertNotIn("silero_vad.onnx", copied)
        self.assertEqual((self.prefix / RUNTIME_MODEL).read_bytes(), b"runtime-fixture")
        current = os.readlink(self.prefix / "current") if (self.prefix / "current").is_symlink() else None
        self.assertNotEqual(current, "releases/m18.6-reuse")

    def test_install_preserves_prefix_and_unit_backup(self):
        installed = self.root / "installed"
        installed.mkdir()
        (installed / "releases/old").mkdir(parents=True)
        (installed / "releases/old/note").write_text("keep")
        (installed / "current").symlink_to("releases/old")
        (installed / "previous-release").write_text("older\n")
        (installed / RUNTIME_MODEL).parent.mkdir(parents=True)
        (installed / RUNTIME_MODEL).write_bytes(b"runtime-fixture")
        install_release(self.prefix, installed, self.release_id)
        self.assertEqual((installed / "releases/old/note").read_text(), "keep")
        self.assertEqual(os.readlink(installed / "current"), "releases/old")
        self.assertEqual((installed / "previous-release").read_text(), "older\n")
        self.assertTrue((installed / "releases" / self.release_id / "bin/firefly-demo").is_file())
        with self.assertRaises(ReleaseError):
            install_release(self.prefix, installed, self.release_id)
        self.assertEqual((installed / "releases/old/note").read_text(), "keep")
        real_copy = release.shutil.copytree

        def full(_source, _dest, **_kwargs):
            raise OSError(errno.ENOSPC, "No space left on device")

        release.shutil.copytree = full
        try:
            with self.assertRaises(ReleaseError) as caught:
                install_release(self.prefix, installed, "m18.6-fixture-b")
        finally:
            release.shutil.copytree = real_copy
        self.assertIn("filesystem is full", str(caught.exception))
        self.assertFalse((installed / "releases" / "m18.6-fixture-b").exists())
        self.assertFalse(any(path.name.startswith(".incoming-") for path in installed.iterdir()))
        self.assertEqual(os.readlink(installed / "current"), "releases/old")
        unit_dir = self.root / "unit"
        unit_dir.mkdir()
        destination = unit_dir / "firefly-voice-assistant.service"
        destination.write_text("OLD checkout unit\n")
        backup_dir = unit_dir / "backups"
        source = self.release / "share/firefly/systemd/firefly-voice-assistant.service"
        backup = install_systemd_unit(source, destination, backup_dir)
        self.assertEqual(Path(backup).read_text(), "OLD checkout unit\n")
        self.assertEqual(destination.read_text(), source.read_text())
        real_replace = os.replace

        def fail_replace(src, dst):
            if str(dst).endswith("firefly-voice-assistant.service"):
                raise OSError("injected replace failure")
            return real_replace(src, dst)

        os.replace = fail_replace
        try:
            with self.assertRaises(OSError):
                install_systemd_unit(source, destination, backup_dir)
        finally:
            os.replace = real_replace
        self.assertEqual(destination.read_text(), source.read_text())
        restore_systemd_unit(Path(backup), destination)
        self.assertEqual(destination.read_text(), "OLD checkout unit\n")
        self.assertTrue((installed / "releases/old/note").is_file())

    def test_rollback_marker_failure_restores_current(self):
        store = self.root / "marker-store"
        secure_store(store, canonical_text())
        before = os.readlink(self.prefix / "current")
        marker = (self.prefix / "previous-release").read_text() if (self.prefix / "previous-release").is_file() else None
        real_write = release._write_previous

        def fail_write(*_args, **_kwargs):
            raise OSError("injected marker failure")

        release._write_previous = fail_write
        try:
            with self.assertRaises(ReleaseError) as caught:
                rollback(self.prefix, store, config_tool=Path(ARGS["config-tool"]))
        finally:
            release._write_previous = real_write
        self.assertIn("restored", str(caught.exception))
        self.assertEqual(os.readlink(self.prefix / "current"), before)
        after = (self.prefix / "previous-release").read_text() if (self.prefix / "previous-release").is_file() else None
        self.assertEqual(after, marker)
        self.assertEqual((store / "configuration.json").read_text(), canonical_text())

    def test_production_staging_rejects_a_dirty_worktree(self):
        source = release.source_identity(Path(ARGS["source"]))
        dirty_source = {**source, "dirty": True, "identity": "dirty-worktree"}
        with patch.object(release, "source_identity", return_value=dirty_source), \
                patch.object(release, "build_identity") as build_probe:
            with self.assertRaises(ReleaseError) as caught:
                stage_release(request(self.root, "m18.6-dirty-production", production=True))
            build_probe.assert_not_called()
        self.assertIn("uncommitted", str(caught.exception))
        self.assertFalse((self.prefix / "releases" / "m18.6-dirty-production").exists())

        clean_source = {**source, "dirty": False, "identity": "committed"}
        clean_build = matching_clean_build_identity(source)
        with patch.object(release, "source_identity", return_value=clean_source), \
                patch.object(release, "build_identity", return_value=clean_build):
            with self.assertRaises(ReleaseError) as caught:
                stage_release(request(self.root, "m18.6-bad-hash", production=True))
        self.assertIn("sha256", str(caught.exception))
        self.assertFalse((self.prefix / "releases" / "m18.6-bad-hash").exists())

    def test_incomplete_configuration_does_not_switch_current(self):
        store = self.root / "incomplete-store"
        secure_store(store, '{"schema_version": 2}\n')
        before = os.readlink(self.prefix / "current")
        with self.assertRaises(ReleaseError) as caught:
            activate(self.prefix, self.release_id, store, config_tool=Path(ARGS["config-tool"]))
        self.assertIn("firefly-config", str(caught.exception))
        self.assertEqual(os.readlink(self.prefix / "current"), before)
        self.assertEqual((store / "configuration.json").read_text(), '{"schema_version": 2}\n')
        pending = self.root / "pending-store"
        secure_store(pending, canonical_text())
        (pending / ".pending.json").write_text("{}\n")
        (pending / ".pending.json").chmod(0o600)
        with self.assertRaises(ReleaseError):
            activate(self.prefix, self.release_id, pending, config_tool=Path(ARGS["config-tool"]))
        self.assertTrue((pending / ".pending.json").is_file())
        self.assertEqual(os.readlink(self.prefix / "current"), before)

    def test_manifest_rejects_model_path_traversal(self):
        outside = self.root / "outside-secret"
        outside.write_text("secret")
        prefix = self.root / "trap-prefix"
        prefix.mkdir()
        for bad in ("/etc/passwd", "../../outside-secret", "models/runtime/../admin/file.rkllm",
                    "models/runtime/evil.rkllm", "models/runtime/file.rkllm"):
            with self.assertRaises(ReleaseError):
                release.assert_shared_model_manifest([{"path": bad}])
        entries = [{"path": path, "sha256": "a" * 64, "bytes": 1} for path in release.ALLOWED_SHARED_MODELS]
        entries.append(dict(entries[0]))
        with self.assertRaises(ReleaseError):
            release.assert_shared_model_manifest(entries)
        self.assertEqual(outside.read_text(), "secret")
        link = prefix / "models"
        link.parent.mkdir(exist_ok=True)
        link.symlink_to(outside.parent)
        with self.assertRaises(ReleaseError):
            release.model_path(prefix, release.RUNTIME_MODEL)
        self.assertEqual(outside.read_text(), "secret")

    def test_install_removes_only_models_published_before_verification_failure(self):
        installed = self.root / "installed-cleanup"
        installed.mkdir()
        (installed / "releases/old").mkdir(parents=True)
        (installed / "releases/old/note").write_text("keep")
        (installed / "current").symlink_to("releases/old")
        secure_parent = installed / RUNTIME_MODEL
        secure_parent.parent.mkdir(parents=True)
        secure_parent.write_bytes(b"runtime-fixture")
        def fail(_created):
            raise ReleaseError("injected verification failure")
        release.install_after_models = fail
        try:
            with self.assertRaises(ReleaseError):
                install_release(self.prefix, installed, self.release_id)
        finally:
            release.install_after_models = None
        self.assertEqual(secure_parent.read_bytes(), b"runtime-fixture")
        self.assertFalse((installed / ADMIN_MODEL).exists())
        self.assertFalse((installed / VAD_MODEL).exists())
        self.assertEqual((installed / "releases/old/note").read_text(), "keep")
        self.assertFalse((installed / "releases" / self.release_id).exists())
        self.assertEqual(os.readlink(installed / "current"), "releases/old")

    def test_opt_install_requires_production_runtime_prefix(self):
        with self.assertRaises(ReleaseError):
            release.assert_production_runtime_prefix("/tmp/review", Path("/opt/firefly/voice-assistant"))
        release.assert_production_runtime_prefix("/opt/firefly/voice-assistant", Path("/tmp/review"))

    def test_missing_stopping_fails_health_gate(self):
        ready = _ready_status()
        self.assertEqual(assess_post_start(ready, "http://127.0.0.1:8080/api/status"), [])
        del ready["stopping"]
        self.assertTrue(assess_post_start(ready, "http://127.0.0.1:8080/api/status"))
        ready = _ready_status()
        del ready["health"]["stopping"]
        self.assertTrue(assess_post_start(ready, "http://127.0.0.1:8080/api/status"))

    def test_stale_binary_is_not_committed_production(self):
        self.assertFalse(self.manifest["production_identity_checked"])
        binary = Path(ARGS["build"]) / "firefly-demo"
        identity = release.build_identity(binary)
        source = release.source_identity(Path(ARGS["source"]))
        matching = matching_clean_build_identity(source)
        self.assertEqual(identity["head"], self.manifest["build"]["head"])
        expected_match = (not source["dirty"] and identity["dirty"] is False and
                          identity["head"] == matching["head"] and
                          identity["source_sha256"] == matching["source_sha256"] and
                          identity["admin_assets_sha256"] == matching["admin_assets_sha256"])
        self.assertEqual(self.manifest["build"]["matches_worktree"], expected_match)

        stale_build = {**matching, "head": "0" * 40}
        clean_source = {**source, "dirty": False, "identity": "committed"}
        with patch.object(release, "build_identity", return_value=stale_build), \
                patch.object(release, "source_identity", return_value=clean_source):
            with self.assertRaises(ReleaseError) as caught:
                stage_release(request(self.root, "m18.6-stale", production=True))
        self.assertIn("fresh firefly-demo", str(caught.exception))
        self.assertFalse((self.prefix / "releases" / "m18.6-stale").exists())

    def test_artifact_manifest_paths_and_duplicates(self):
        for bad in ("/etc/passwd", "../outside", "bin/../bin/firefly-demo", "unlisted-file",
                    release.SITE_PREFIX + "../../escape", "bin/firefly-demo\x00"):
            entries = [dict(item) for item in self.manifest["artifacts"]]
            entries[0]["path"] = bad
            with self.assertRaises(ReleaseError):
                release.assert_artifact_manifest(entries)
        with self.assertRaises(ReleaseError):
            release.assert_artifact_manifest(self.manifest["artifacts"] + [self.manifest["artifacts"][0]])

    def test_release_symlinks_special_files_and_empty_directories(self):
        alias = self.prefix / "releases/alias"
        alias.symlink_to(self.release)
        try:
            with self.assertRaises(ReleaseError):
                verify_tree(self.prefix, "alias", check_python=False)
        finally:
            alias.unlink()
        entry = self.release / "unexpected"
        for kind in ("symlink", "fifo", "directory"):
            if kind == "symlink": entry.symlink_to(self.root)
            elif kind == "fifo": os.mkfifo(entry)
            else: entry.mkdir()
            try:
                with self.assertRaises(ReleaseError):
                    verify_tree(self.prefix, self.release_id, check_python=False)
            finally:
                if kind == "directory": entry.rmdir()
                else: entry.unlink()

    def test_install_rejects_symlinked_release_parent(self):
        destination = self.root / "symlink-install"
        outside = self.root / "outside-releases"
        destination.mkdir(); outside.mkdir()
        (outside / "keep").write_text("keep")
        (destination / "releases").symlink_to(outside)
        with self.assertRaises(ReleaseError):
            install_release(self.prefix, destination, self.release_id)
        self.assertEqual(list(outside.iterdir()), [outside / "keep"])
        self.assertEqual((outside / "keep").read_text(), "keep")

    def test_atomic_directory_publication_never_overwrites(self):
        source, target = self.root / "rename-source", self.root / "rename-target"
        source.mkdir(); target.mkdir()
        (source / "marker").write_text("new")
        with self.assertRaises(FileExistsError):
            release._publish_directory(source, target)
        self.assertTrue((source / "marker").is_file())
        self.assertEqual(list(target.iterdir()), [])

    def test_install_preserves_preexisting_scratch_directory(self):
        destination = self.root / "scratch-install"
        destination.mkdir()
        scratch = destination / f".incoming-{self.release_id}-{os.getpid()}"
        scratch.mkdir(); (scratch / "keep").write_text("keep")
        install_release(self.prefix, destination, self.release_id)
        self.assertEqual((scratch / "keep").read_text(), "keep")

    def test_stage_failure_after_model_publication_preserves_existing_data(self):
        root = self.root / "stage-cleanup"
        write_fixtures(root)
        destination = root / "prefix"
        old = destination / "releases/old"
        old.mkdir(parents=True); (old / "keep").write_text("keep")
        scratch = destination / f".staging-cleanup-{os.getpid()}"
        scratch.mkdir(); (scratch / "keep").write_text("scratch")
        real_verify = release.verify_tree
        def fail_final(prefix, *args, **kwargs):
            document = real_verify(prefix, *args, **kwargs)
            if prefix == destination:
                raise ReleaseError("injected post-publication failure")
            return document
        release.verify_tree = fail_final
        try:
            with self.assertRaises(ReleaseError):
                stage_release(request(root, "cleanup"))
        finally:
            release.verify_tree = real_verify
        self.assertFalse((destination / "releases/cleanup").exists())
        for relative in release.ALLOWED_SHARED_MODELS:
            self.assertFalse((destination / relative).exists())
        self.assertEqual((old / "keep").read_text(), "keep")
        self.assertEqual((scratch / "keep").read_text(), "scratch")

    def test_dirty_build_cannot_be_relabelled_as_clean(self):
        real_source = release.source_identity
        real_build = release.build_identity
        release.source_identity = lambda _root: {
            "commit": self.manifest["source_commit"], "dirty": False, "identity": "committed"}
        def dirty_build(binary):
            found = real_build(binary); found["dirty"] = True
            return found
        release.build_identity = dirty_build
        try:
            with self.assertRaises(ReleaseError) as caught:
                stage_release(request(self.root, "dirty-build", production=True))
        finally:
            release.source_identity, release.build_identity = real_source, real_build
        self.assertIn("fresh firefly-demo", str(caught.exception))
        self.assertFalse((self.prefix / "releases/dirty-build").exists())

    def test_fixture_cannot_be_installed_as_production(self):
        with self.assertRaises(ReleaseError):
            release.require_production_identity(self.manifest, self.release)

    def test_runtime_prefix_rejects_systemd_injection(self):
        for prefix in ("/tmp/release\nUser=root", "/tmp/../etc", "/tmp/space path", "/tmp/%h", "/tmp//bad"):
            with self.assertRaises(ReleaseError): render_unit(prefix)

    def test_failed_regular_copy_removes_its_partial_file(self):
        directory = self.root / "copy-cleanup"
        directory.mkdir()
        target = directory / "target"
        real_read = os.read
        os.read = lambda *_args: (_ for _ in ()).throw(OSError(errno.ENOSPC, "injected full disk"))
        try:
            with self.assertRaises(OSError):
                release._copy_regular(self.root / "inputs/runtime.rkllm", target)
        finally:
            os.read = real_read
        self.assertEqual(list(directory.iterdir()), [])

    def test_writable_prefix_and_concurrent_mutation_are_rejected(self):
        prefix = self.root / "unsafe-prefix"
        prefix.mkdir(); prefix.chmod(0o777)
        with self.assertRaises(ReleaseError): release._lock_prefix(prefix)
        prefix.chmod(0o755)
        lock = release._lock_prefix(prefix)
        try:
            with self.assertRaises(BlockingIOError): release._lock_prefix(prefix)
        finally:
            os.close(lock)

    def test_unit_diagnostics_with_filename_and_line_number_are_defects(self):
        report = classify_systemd_analyze(
            "/tmp/firefly-voice-assistant.service:32: Unknown key name 'Bad' in section 'Service'\n"
            "Failed to load unit: Invalid argument\n")
        self.assertEqual(len(report["defects"]), 2)

    def test_wake_wrapper_ignores_developer_python_environment(self):
        python = self.release / "share/firefly/wake/python/bin/python"
        env = {**os.environ, "PYTHONPATH": "/home/firefly/injected", "PYTHONHOME": "/nonexistent"}
        result = subprocess.run([str(python), "-c", "import sys; print(sys.path)"],
                                env=env, cwd="/tmp", capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("/home/firefly/injected", result.stdout)

    def test_admin_model_fifo_does_not_block_startup_validation(self):
        fifo = self.root / "admin-model-fifo"
        os.mkfifo(fifo)
        # No --store: exits before runtime/voice initialization or any inference.
        result = subprocess.run([str(self.release / "bin/firefly-demo"), "--admin-model", str(fifo)],
                                capture_output=True, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("admin_model_unavailable_or_wrong_size", result.stderr)
        self.assertIn("--store is required", result.stderr)

    def test_standalone_release_tool_and_target_config_loader(self):
        store = self.root / "standalone-store"
        secure_store(store, canonical_text())
        before = snapshot(store)
        tool = self.release / "share/firefly/deployment/scripts/stage-release.py"
        env = {**os.environ, "PYTHONPATH": "", "LD_LIBRARY_PATH": ""}
        result = subprocess.run([sys.executable, str(tool), "activate", "--destination", str(self.prefix),
                                 "--release-id", self.release_id, "--store", str(store)],
                                cwd="/tmp", env=env, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(snapshot(store), before)
        self.assertFalse(any(self.release.rglob("__pycache__")))

    def test_unexpected_shared_model_entries_are_rejected(self):
        root = self.root / "unexpected-models"
        (root / "models/runtime").mkdir(parents=True)
        extra = root / "models/runtime/unlisted.rkllm"
        extra.write_text("keep")
        with self.assertRaises(ReleaseError): release.assert_model_tree(root)
        self.assertEqual(extra.read_text(), "keep")

    def test_store_audit_rejects_links_special_files_and_noncanonical_bytes(self):
        store = self.root / "audit-fixtures"
        config = secure_store(store, canonical_text())
        baseline = snapshot(store)
        require_store(store, [1,2], config_tool=Path(ARGS["config-tool"]))
        self.assertEqual(snapshot(store), baseline)
        self.assertEqual(sorted(path.name for path in store.iterdir()), ["configuration.json"])
        config.unlink()
        target = self.root / "audit-target.json"; target.write_text(canonical_text()); target.chmod(0o600)
        for kind in ("symlink", "hardlink", "fifo", "noncanonical"):
            if kind == "symlink": config.symlink_to(target)
            elif kind == "hardlink": os.link(target, config)
            elif kind == "fifo": os.mkfifo(config)
            else: config.write_text(" " + canonical_text()); config.chmod(0o600)
            try:
                with self.assertRaises(ReleaseError):
                    require_store(store, [1,2], config_tool=Path(ARGS["config-tool"]))
            finally:
                config.unlink()
            self.assertEqual(target.read_text(), canonical_text())
        self.assertEqual(list(store.iterdir()), [])

    def test_stale_config_auditor_rejects_staging(self):
        real_build = release.build_identity
        def stale_config(binary):
            found = real_build(binary)
            if binary.name == "firefly-config": found["source_sha256"] = "0" * 64
            return found
        release.build_identity = stale_config
        try:
            with self.assertRaises(ReleaseError) as caught:
                stage_release(request(self.root, "stale-config"))
        finally:
            release.build_identity = real_build
        self.assertIn("same build identity", str(caught.exception))
        self.assertFalse((self.prefix / "releases/stale-config").exists())

    def test_manifest_checksum_covers_exact_bytes(self):
        directory = self.root / "crlf-manifest"
        directory.mkdir()
        body = b'{\n"format": 1,\n"release_id": "crlf-manifest"\n}\n'
        (directory / "manifest.json").write_bytes(body.replace(b'\n', b'\r\n'))
        (directory / "manifest.sha256").write_text(hashlib.sha256(body).hexdigest()+'\n')
        with self.assertRaises(ReleaseError): release.load_manifest(directory)

    def test_production_store_is_the_unit_store_owned_by_service_user(self):
        store = self.root / "production-store-contract"
        secure_store(store, canonical_text())
        with self.assertRaises(ReleaseError): release.require_production_store(store)
        real_live, real_user = release.LIVE_STORE, release.pwd.getpwnam
        release.LIVE_STORE = store
        try:
            release.pwd.getpwnam = lambda _name: type("User", (), {"pw_uid": os.geteuid()})()
            release.require_production_store(store)
            with self.assertRaises(ReleaseError): release.require_production_store(store, fresh=True)
            release.pwd.getpwnam = lambda _name: type("User", (), {"pw_uid": os.geteuid()+1})()
            with self.assertRaises(ReleaseError): release.require_production_store(store)
        finally:
            release.LIVE_STORE, release.pwd.getpwnam = real_live, real_user

    def test_root_store_audit_preserves_bytes_and_rejects_owner_mismatch(self):
        # This privilege is confined to a new temporary directory. It proves
        # root can audit the service-owned store without relaxing its contract.
        script = '''
import os, pathlib, pwd, subprocess, sys, tempfile
assert os.geteuid() == 0
user = pwd.getpwnam("firefly")
body = pathlib.Path(sys.argv[2]).read_bytes()
with tempfile.TemporaryDirectory(prefix="firefly-root-audit-") as directory:
    store = pathlib.Path(directory)
    os.chown(store, user.pw_uid, user.pw_gid)
    store.chmod(0o700)
    config = store / "configuration.json"
    config.write_bytes(body); config.chmod(0o600)
    os.chown(config, user.pw_uid, user.pw_gid)
    before = config.stat()
    valid = subprocess.run([sys.argv[1], "audit", "--store", str(store)], capture_output=True, text=True, timeout=10)
    assert valid.returncode == 0, valid.stderr
    after = config.stat()
    assert (after.st_uid, after.st_mode, after.st_mtime_ns) == (before.st_uid, before.st_mode, before.st_mtime_ns)
    assert config.read_bytes() == body and sorted(p.name for p in store.iterdir()) == ["configuration.json"]
    os.chown(config, 0, 0)
    invalid = subprocess.run([sys.argv[1], "audit", "--store", str(store)], capture_output=True, text=True, timeout=10)
    assert invalid.returncode != 0 and "unsafe_store" in invalid.stderr, invalid.stderr
    assert config.stat().st_uid == 0 and config.read_bytes() == body
    assert sorted(p.name for p in store.iterdir()) == ["configuration.json"]
print("PASS root read-only audit and owner mismatch")
'''
        command = ["/usr/bin/python3", "-I", "-c", script,
                   str(self.release / "bin/firefly-config"), str(Path(ARGS["source"]) / "config/empty-home.json")]
        if os.geteuid() != 0:
            command = ["sudo", "-n", *command]
        result = subprocess.run(command, cwd="/tmp", capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_production_prefix_cannot_be_nested_below_unit_paths(self):
        real_owner = release._owner_accepted
        release._owner_accepted = lambda: True
        try:
            with self.assertRaises(ReleaseError):
                assert_activation_destination(Path(PRODUCTION_PREFIX) / "nested")
        finally:
            release._owner_accepted = real_owner

    def test_unit_and_backup_paths_cannot_write_other_protected_roots(self):
        for path in ("/etc/other.service", "/var/lib/firefly/store/unit", "/opt/other/unit",
                     "/home/firefly/FireflyVoiceAssistantLinux/packaging/unit",
                     "/home/firefly/demo_Linux_aarch64/unit"):
            with self.assertRaises(ReleaseError): release._assert_not_live_unit(Path(path))
            with self.assertRaises(ReleaseError): release._assert_unit_destination(Path(path), backup=True)

    def test_unit_backup_race_preserves_existing_archive_and_unit(self):
        directory = self.root / "unit-backup-race"
        directory.mkdir()
        destination = directory / "firefly-voice-assistant.service"
        destination.write_text("OLD unit\n")
        source = self.release / "share/firefly/systemd/firefly-voice-assistant.service"
        backups = directory / "backups"
        real_link = os.link
        def raced_link(snapshot, backup, *args, **kwargs):
            Path(backup).write_text("existing rollback archive\n")
            raise FileExistsError(backup)
        os.link = raced_link
        try:
            with self.assertRaises(FileExistsError): install_systemd_unit(source, destination, backups)
        finally:
            os.link = real_link
        self.assertEqual(destination.read_text(), "OLD unit\n")
        self.assertEqual([path.read_text() for path in backups.iterdir()], ["existing rollback archive\n"])


if __name__ == "__main__":
    unittest.main()
