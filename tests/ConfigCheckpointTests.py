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
