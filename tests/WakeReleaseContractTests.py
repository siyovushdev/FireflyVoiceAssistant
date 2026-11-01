"""Pinned old/new wake releases, promotion and CMake layout; no live hardware."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE))
from packaging import firefly_release as release
spec = importlib.util.spec_from_file_location('wake_release_runner', SOURCE / 'scripts/release-runner.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def fixture(root, version):
    wake = root / 'share/firefly/wake'
    wake.mkdir(parents=True)
    current = version == release.AMINA_VERSION
    shutil.copyfile(SOURCE / (release.AMINA_SOURCE if current else 'assets/wake/amina-v3.tflite'),
                    wake / 'amina-v3.tflite')
    worker = (SOURCE / 'runtime/wake/amina_worker.py').read_text()
    if not current:
        # This revision changes the SHA constant only: recover exact old worker
        # bytes for the legacy-package regression, without downloading Git data.
        worker = worker.replace(release.AMINA_SHA256, release.LEGACY_AMINA_SHA256)
    (wake / 'amina_worker.py').write_text(worker)
    shutil.copyfile(SOURCE / ('assets/wake/manifest.json' if current else
                             'tests/fixtures/wake-v3-manifest.json'), wake / 'manifest.json')
    for name, content in [('lib/frozen.so', b'unchanged'), ('share/firefly/config/empty-home.json', b'{}'),
                          ('share/firefly/systemd/firefly-voice-assistant.service', b'frozen unit\n')]:
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    entries = []
    for p in sorted(root.rglob('*')):
        if p.is_file():
            p.chmod(0o644)
            entries.append(dict(path=str(p.relative_to(root)), sha256=hashlib.sha256(p.read_bytes()).hexdigest(),
                                bytes=p.stat().st_size, mode=0o644))
    return dict(artifacts=entries, shared_models=[dict(path='models/unchanged', sha256='f' * 64)],
                dependencies=dict(amina_wake=version, amina_sha256=release.AMINA_RELEASES[version]['model'][0]))


class WakeReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='firefly-wake-release-')
        self.root = Path(self.temp.name)
        self.old, self.new = self.root / 'old', self.root / 'new'
        self.before = fixture(self.old, 'v3')
        self.after = fixture(self.new, release.AMINA_VERSION)

    def tearDown(self):
        self.temp.cleanup()

    def test_exact_legacy_and_current_pairs_remain_verifiable(self):
        self.assertEqual(release.verify_wake_assets(self.old, self.before), 'v3')
        self.assertEqual(release.verify_wake_assets(self.new, self.after), release.AMINA_VERSION)

    def test_promoted_bytes_are_the_source_selected_in_manifest(self):
        m = json.loads((SOURCE / 'assets/wake/manifest.json').read_text())
        p = SOURCE / 'assets/wake' / m['source_file']
        self.assertEqual((m['sha256'], m['bytes']), (release.AMINA_SHA256, release.AMINA_BYTES))
        self.assertEqual(hashlib.sha256(p.read_bytes()).hexdigest(), m['sha256'])
        self.assertEqual(hashlib.sha256((SOURCE / 'assets/wake/amina-v3.tflite').read_bytes()).hexdigest(), release.LEGACY_AMINA_SHA256)

    def test_mixed_model_worker_or_metadata_is_rejected(self):
        for relative in [release.WAKE_MODEL, 'share/firefly/wake/amina_worker.py', 'share/firefly/wake/manifest.json']:
            with self.subTest(relative=relative):
                path = self.new / relative
                original = path.read_bytes()
                path.write_bytes((self.old / relative).read_bytes())
                with self.assertRaises(release.ReleaseError):
                    release.verify_wake_assets(self.new, self.after)
                path.write_bytes(original)

    def test_invented_revision_and_tampering_are_rejected(self):
        for key, value in [('amina_wake', 'anything'), ('amina_sha256', '0' * 64)]:
            bad = copy.deepcopy(self.after)
            bad['dependencies'][key] = value
            with self.assertRaises(release.ReleaseError):
                release.verify_wake_assets(self.new, bad)
        path = self.new / release.WAKE_MODEL
        path.write_bytes(path.read_bytes()[:-1] + b'X')
        with self.assertRaises(release.ReleaseError):
            release.verify_wake_assets(self.new, self.after)

    def test_only_the_exact_coupled_upgrade_preserves_other_frozen_files(self):
        runner.preservation_contract(self.before, self.after, self.old, self.new)
        for fault in ['model', 'worker', 'manifest', 'other', 'remove', 'extra', 'shared', 'property',
                      'mode', 'revision', 'physical', 'incomplete', 'old-identity']:
            with self.subTest(fault=fault):
                bad = copy.deepcopy(self.after)
                target = self.new / release.WAKE_MODEL
                original = target.read_bytes()
                if fault in ['model', 'worker', 'manifest', 'other']:
                    suffix = {'model': 'amina-v3.tflite', 'worker': 'amina_worker.py',
                              'manifest': 'manifest.json', 'other': 'frozen.so'}[fault]
                    next(e for e in bad['artifacts'] if e['path'].endswith(suffix))['sha256'] = '0' * 64
                elif fault == 'remove': bad['artifacts'].pop(0)
                elif fault == 'extra': bad['artifacts'].append(dict(path='lib/unknown.so'))
                elif fault == 'shared': bad['shared_models'][0]['sha256'] = '0' * 64
                elif fault == 'property': next(e for e in bad['artifacts'] if e['path'] == release.WAKE_MODEL)['extra'] = True
                elif fault == 'mode': target.chmod(0o666)
                elif fault == 'revision': bad['dependencies']['amina_wake'] = 'unapproved'
                elif fault == 'physical': target.write_bytes(b'X' * len(original))
                elif fault == 'incomplete': bad['artifacts'] = [e for e in bad['artifacts'] if not e['path'].endswith('amina_worker.py')]
                elif fault == 'old-identity': bad['dependencies']['amina_sha256'] = release.LEGACY_AMINA_SHA256
                with self.assertRaises(RuntimeError):
                    runner.preservation_contract(self.before, bad, self.old, self.new)
                target.write_bytes(original)
                target.chmod(0o644)

    def test_promotion_is_not_a_general_permission_to_change_later_wake_bytes(self):
        other = self.root / 'other'
        same = fixture(other, release.AMINA_VERSION)
        runner.preservation_contract(self.after, same, self.new, other)
        next(e for e in same['artifacts'] if e['path'] == release.WAKE_MODEL)['sha256'] = '0' * 64
        with self.assertRaises(RuntimeError):
            runner.preservation_contract(self.after, same, self.new, other)

    def test_clean_cmake_build_data_and_component_install_select_promoted_model(self):
        build, stage = self.root / 'build', self.root / 'stage'
        before = (SOURCE / 'assets/wake/amina-v3.tflite').stat()
        env = dict(os.environ)
        for key in ['DISPLAY', 'FIREFLY_OWNER_ACCEPTED', 'LD_PRELOAD', 'LD_LIBRARY_PATH']:
            env.pop(key, None)
        command = ['cmake', '-S', str(SOURCE), '-B', str(build), '-DBUILD_TESTING=ON',
                   '-DFIREFLY_HOST_OFFLINE_TESTS=ON', '-DFIREFLY_BUILD_ASSISTANT=OFF', '-DFIREFLY_BUILD_VOICE=OFF',
                   '-DFIREFLY_BUILD_ADMIN=OFF', '-DFIREFLY_BUILD_CONFIG_TOOLS=OFF']
        for argv in [command, ['cmake', '--install', str(build), '--prefix', str(stage), '--component', 'firefly-runtime']]:
            result = subprocess.run(argv, env=env, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(release.verify_wake_assets(build), release.AMINA_VERSION)
        self.assertEqual(release.verify_wake_assets(stage), release.AMINA_VERSION)
        self.assertFalse((stage / release.WAKE_MODEL).is_symlink())
        after = (SOURCE / 'assets/wake/amina-v3.tflite').stat()
        self.assertEqual((before.st_ino, before.st_mtime_ns, before.st_ctime_ns),
                         (after.st_ino, after.st_mtime_ns, after.st_ctime_ns))


if __name__ == '__main__':
    unittest.main(verbosity=2)
