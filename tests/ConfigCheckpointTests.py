"""Private whole-store transaction tests; no services, pointers, models or hardware."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from packaging import firefly_checkpoint as c
APP = Path(sys.argv[1]).resolve()
sys.argv = [sys.argv[0]]


class CheckpointTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='firefly-checkpoint-test-')
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.app = self.base / 'config'
        shutil.copy2(APP, self.app)
        self.app.chmod(0o755)
        # Transaction mechanics run on the CMake-supplied native config tool.
        # Real original-M19 semantic proof is a separate archived-ELF fixture.
        authority = patch.object(c, 'M19_CONFIG_SHA256', c.digest(self.app))
        authority.start(); self.addCleanup(authority.stop)
        self.store = self.base / 'store'; self.store.mkdir(mode=0o700)
        self.file = self.base / 'input.json'
        self.cfg = json.loads((ROOT / 'tests/admin/v2/system.json').read_text())
        self.file.write_text(json.dumps(self.cfg))
        self.call('init', '--store', self.store, '--file', self.file)
        self.save('before checkpoint')
        # Representative genuine ConfigStore LKG representation + opaque metadata.
        snapshot = self.store / 'last_known_good_snapshot.json'
        shutil.copy2(self.store / 'configuration.json', snapshot)
        audit = self.call('audit', '--store', self.store)
        marker = self.store / 'last_known_good.json'
        marker.write_text(json.dumps(dict(revision=audit['revision'], hash=audit['content_sha256'])))
        marker.chmod(0o600)
        (self.store / 'operator-metadata').write_bytes(b'preserve exact metadata\x00')
        (self.store / 'operator-metadata').chmod(0o600)
        self.checkpoint = self.base / 'checkpoint'
        self.pin = c.create(self.store, self.checkpoint, self.app)
        self.original = c.inventory(self.store)
        self.save('M20 later state')
        self.newer = c.inventory(self.store)
        self.transaction = self.base / 'rollback-transaction'

    def call(self, *args):
        p = subprocess.run([str(self.app), *map(str, args)], capture_output=True, text=True, check=True)
        return json.loads(p.stdout)

    def save(self, name):
        self.cfg['system']['name'] = name
        self.file.write_text(json.dumps(self.cfg))
        a = self.call('audit', '--store', self.store)
        return self.call('save', '--store', self.store, '--file', self.file,
                         '--expected-revision', a['revision'], '--expected-hash', a['content_sha256'])

    def restore(self, **kw):
        return c.restore(self.store, self.checkpoint, self.app, self.pin, self.transaction, **kw)

    def test_complete_tree_and_full_old_operations_then_idempotent_retry(self):
        self.assertEqual(self.restore()['result'], 'RESTORED_VERIFIED')
        self.assertEqual(c.inventory(self.store), self.original)
        self.assertEqual(c.inventory(self.transaction / 'preserved-store'), self.newer)
        self.call('revisions', '--store', self.store)
        self.save('old CAS after rollback')
        self.assertTrue(self.restore()['retry'])
        self.assertEqual(c.inventory(self.transaction / 'preserved-store'), self.newer)

    def test_actual_process_crashes_on_both_sides_of_exchange_are_retryable(self):
        for point in ['before_exchange', 'after_exchange', 'after_sync', 'before_receipt']:
            with self.subTest(point=point):
                self.transaction = self.base / ('transaction-' + point)
                pid = os.fork()
                if pid == 0:
                    try:
                        self.restore(fault=lambda step: os._exit(73) if step == point else None)
                    except BaseException:
                        os._exit(91)
                    os._exit(92)
                _, status = os.waitpid(pid, 0)
                self.assertEqual(os.waitstatus_to_exitcode(status), 73)
                self.assertIn(c.inventory(self.store), [self.newer, self.original])
                self.assertEqual(self.restore()['result'], 'RESTORED_VERIFIED')
                self.assertEqual(c.inventory(self.store), self.original)
                self.assertEqual(c.inventory(self.transaction / 'preserved-store'), self.newer)
                self.save('M20 later state')
                # New CAS revision is deliberately captured for the next transaction.
                self.newer = c.inventory(self.store)

    def test_incomplete_or_corrupt_checkpoint_does_not_exchange(self):
        path = self.checkpoint / 'store/backups'
        backup = next(path.iterdir()); original = backup.read_bytes()
        for fault in ['missing', 'corrupt', 'symlink', 'hardlink', 'mode', 'pending']:
            with self.subTest(fault=fault):
                if fault == 'missing': backup.rename(path / 'unexpected')
                elif fault == 'corrupt': backup.write_bytes(b'bad json')
                elif fault == 'symlink': backup.unlink(); backup.symlink_to(self.file)
                elif fault == 'hardlink': os.link(backup, self.base / 'alias')
                elif fault == 'mode': backup.chmod(0o666)
                else: (self.checkpoint / 'store/.pending.json').write_text('{}')
                with self.assertRaises((c.CheckpointError, OSError)):
                    self.restore()
                self.assertEqual(c.inventory(self.store), self.newer)
                self.assertFalse(self.transaction.exists())
                if fault == 'missing': (path / 'unexpected').rename(backup)
                elif fault in ['corrupt', 'symlink']:
                    backup.unlink(); backup.write_bytes(original); backup.chmod(0o600)
                elif fault == 'hardlink': (self.base / 'alias').unlink()
                elif fault == 'mode': backup.chmod(0o600)
                else: (self.checkpoint / 'store/.pending.json').unlink()
                # Restore exact checkpoint times from its pinned metadata.
                for name, entry in self.original.items():
                    q = self.checkpoint / 'store' / name
                    os.utime(q, ns=(entry['mtime_ns'], entry['mtime_ns']))

    def test_interrupted_staging_and_wrong_pin_fail_closed_without_history_loss(self):
        with self.assertRaises(c.CheckpointError):
            c.restore(self.store, self.checkpoint, self.app, '0' * 64, self.transaction)
        def fail(step):
            if step == 'before_exchange': raise OSError('injected failure')
        with self.assertRaises(OSError): self.restore(fault=fail)
        (self.transaction / 'restored-store/configuration.json').write_bytes(b'incomplete')
        with self.assertRaisesRegex(c.CheckpointError, 'Incomplete'):
            self.restore()
        self.assertEqual(c.inventory(self.store), self.newer)
        self.transaction = self.base / 'fresh-retry'
        self.assertEqual(self.restore()['result'], 'RESTORED_VERIFIED')

    def test_bad_lkg_and_wrong_m19_authority_never_publish(self):
        for kind in ['wrong-binary', 'boolean-revision', 'noncanonical-snapshot', 'missing-marker']:
            with self.subTest(kind=kind):
                target = self.base / ('bad-' + kind)
                source = self.base / ('source-' + kind)
                c.clone(self.checkpoint / 'store', source)
                if kind == 'boolean-revision':
                    marker = source / 'last_known_good.json'
                    doc = json.loads(marker.read_bytes()); doc['revision'] = True; marker.write_text(json.dumps(doc))
                elif kind == 'noncanonical-snapshot':
                    snapshot = source / 'last_known_good_snapshot.json'; snapshot.write_bytes(snapshot.read_bytes() + b' ')
                elif kind == 'missing-marker': (source / 'last_known_good.json').unlink()
                authority = '0' * 64 if kind == 'wrong-binary' else c.digest(self.app)
                before = c.inventory(source)
                with patch.object(c, 'M19_CONFIG_SHA256', authority), self.assertRaises(c.CheckpointError):
                    c.create(source, target, self.app)
                self.assertFalse(target.exists()); self.assertEqual(c.inventory(source), before)

    def test_creation_fsync_failure_and_unauthorized_production_restore_fail_closed(self):
        with patch.object(c.os, 'fsync', side_effect=OSError('fsync failed')):
            with self.assertRaises(OSError):
                c.create(self.store, self.base / 'fsync-failed', self.app)
        self.assertFalse((self.base / 'fsync-failed').exists())
        with patch.dict(os.environ, {}, clear=True), patch.object(c, 'inventory') as inspected:
            with self.assertRaisesRegex(c.CheckpointError, 'owner authorization'):
                c.quiescent(Path('/var/lib/firefly/store'))
            inspected.assert_not_called()

    def test_activation_baselines_reject_volatile_or_hidden_tmpfs(self):
        for path in ['/run/checkpoint', '/tmp/checkpoint', '/dev/shm/checkpoint']:
            with self.assertRaises(c.CheckpointError): c.require_persistent(Path(path))
        mountinfo = ('1 0 8:1 / / rw - xfs /dev/root rw\n'
                     '2 1 0:52 / /owner rw - tmpfs tmpfs rw\n')
        info = type('Info', (), {'st_dev': os.makedev(0, 52)})()
        with patch.object(Path, 'exists', return_value=False), patch.object(Path, 'stat', return_value=info), \
             patch.object(Path, 'read_text', return_value=mountinfo):
            with self.assertRaises(c.CheckpointError): c.require_persistent(Path('/owner/checkpoint'))

    def test_durable_capsule_binds_original_m19_complete_store_and_unit(self):
        receipt = c.control(self.checkpoint / 'checkpoint.json')
        audit = receipt['proof']['current']; expected = dict(revision=audit['revision'], hash=audit['content_sha256'])
        tree = self.base / 'original-m19'
        (tree / 'bin').mkdir(parents=True)
        (tree / 'bin/firefly-demo').write_text('LABELLED fake mode fixture')
        (tree / 'bin/firefly-demo').chmod(0o755)
        (tree / 'manifest.json').write_text(json.dumps({'artifacts': [{'path': 'bin/firefly-demo'}]}))
        data = dict(format=1, checkpoint_pin=self.pin, release=tree.name,
                    identity=dict(head=c.M19_HEAD, dirty=False), binary_sha256=c.M19_DEMO_SHA256,
                    manifest_sha256='a' * 64, artifact_modes={'bin/firefly-demo': 0o755},
                    before=dict(status=dict(stored=expected, active={**expected, 'generation_id': 2}),
                                argv=['/opt/firefly/voice-assistant/current/bin/firefly-demo', ''],
                                unit_sha256='b' * 64, store_manifest=c.legacy_inventory(receipt['tree'])))
        def digest(path):
            return {'firefly-demo': c.M19_DEMO_SHA256, 'firefly-config': c.M19_CONFIG_SHA256,
                    'manifest.json': 'a' * 64, 'firefly-voice-assistant.service': 'b' * 64}[path.name]
        with patch.object(c, 'digest', side_effect=digest):
            c.validate_capsule(data, receipt, self.pin, tree)
            for fault in ['checkpoint', 'head', 'binary', 'manifest', 'stored', 'active', 'generation', 'unit', 'metadata', 'argv', 'artifact-mode', 'missing-artifact-mode']:
                bad = copy.deepcopy(data)
                if fault == 'checkpoint': bad['checkpoint_pin'] = '0' * 64
                elif fault == 'head': bad['identity']['head'] = 'unreviewed'
                elif fault == 'binary': bad['binary_sha256'] = '0' * 64
                elif fault == 'manifest': bad['manifest_sha256'] = '0' * 64
                elif fault == 'stored': bad['before']['status']['stored']['revision'] += 1
                elif fault == 'active': bad['before']['status']['active']['hash'] = '0' * 64
                elif fault == 'generation': bad['before']['status']['active']['generation_id'] = True
                elif fault == 'unit': bad['before']['unit_sha256'] = '0' * 64
                elif fault == 'metadata': bad['before']['store_manifest'].pop('.lock')
                elif fault == 'artifact-mode': bad['artifact_modes']['bin/firefly-demo'] = 0o6755
                elif fault == 'missing-artifact-mode': bad['artifact_modes'].clear()
                else: bad['before']['argv'][0] = '/unreviewed/demo'
                with self.subTest(fault=fault), self.assertRaises(c.CheckpointError):
                    c.validate_capsule(bad, receipt, self.pin, tree)

    def test_checkpoint_rejects_pending_and_lock_contention(self):
        (self.store / '.pending.json').write_text('{}')
        with self.assertRaises(c.CheckpointError):
            c.create(self.store, self.base / 'bad-checkpoint', self.app)
        self.assertFalse((self.base / 'bad-checkpoint').exists())
        (self.store / '.pending.json').unlink()
        with c.locked(self.store), self.assertRaises(BlockingIOError):
            c.create(self.store, self.base / 'blocked-checkpoint', self.app)


if __name__ == '__main__':
    unittest.main(verbosity=2)
