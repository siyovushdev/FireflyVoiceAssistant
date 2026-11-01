#!/usr/bin/env python3
"""File-only reference safety: no installed paths, device access or large models."""
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from packaging import firefly_release as r
REAL_OWNER_GUARD = r.require_root_owned

class ModelReferenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name) / 'installed'
        self.release = Path(self.temp.name) / 'staging/releases/new'
        self.release.mkdir(parents=True)
        self.paths = {}
        self.hashes = {}
        for name in r.ALLOWED_SHARED_MODELS:
            p = self.root / name; p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(name.encode()); self.paths[name] = p
            self.hashes[name] = hashlib.sha256(p.read_bytes()).hexdigest()
        self.target = self.root / 'releases/old' / r.WAKE_MODEL
        self.target.parent.mkdir(parents=True)
        self.target.write_bytes((Path(__file__).resolve().parents[1] / 'assets/wake/amina-v3.tflite').read_bytes())
        self.link = self.release / r.WAKE_MODEL; self.link.parent.mkdir(parents=True)
        self.link.symlink_to(self.target)
        self.patches = [patch.object(r, 'PRODUCTION_PREFIX', str(self.root)),
                        patch.object(r, 'PRODUCTION_SHA256', self.hashes),
                        patch.object(r, 'ADMIN_MODEL_BYTES', self.paths[r.ADMIN_MODEL].stat().st_size),
                        # Fixtures belong to the invoking user, not root. Real owner guard separately tested.
                        patch.object(r, 'require_root_owned')]
        for p in self.patches: p.start()
    def tearDown(self):
        for p in reversed(self.patches): p.stop()
        self.temp.cleanup()
    def marker(self):
        return {'installed_model_reference': {'prefix': str(self.root), 'mode':'installed-read-only'},
                'runtime_prefix':str(self.root), 'production_identity_checked':True,
                'wake_model_reference':str(self.target)}
    def test_fixed_installed_set_has_no_copies_or_metadata_changes(self):
        before = {n:p.stat() for n,p in self.paths.items()}
        self.assertEqual(r.manifest_model_paths(self.marker(), self.release), self.paths)
        for n,p in self.paths.items():
            self.assertEqual((p.stat().st_ino,p.stat().st_nlink,p.stat().st_mtime_ns,p.stat().st_ctime_ns),
                             (before[n].st_ino,before[n].st_nlink,before[n].st_mtime_ns,before[n].st_ctime_ns))
        self.assertFalse((self.release/'models').exists())
    def test_missing_tampered_symlink_and_hardlink_models_fail_closed(self):
        path = self.paths[r.VAD_MODEL]; original = path.read_bytes()
        path.write_bytes(b'tampered')
        with self.assertRaises(r.ReleaseError): r.installed_model_paths()
        path.unlink()
        with self.assertRaises(r.ReleaseError): r.installed_model_paths()
        path.symlink_to(self.paths[r.RUNTIME_MODEL])
        with self.assertRaises(r.ReleaseError): r.installed_model_paths()
        path.unlink(); path.write_bytes(original)
        os.link(path, self.root/'alias')
        with self.assertRaises(r.ReleaseError): r.installed_model_paths()
    def test_arbitrary_or_fixture_reference_contract_is_rejected(self):
        for key,value in [('runtime_prefix','/tmp/elsewhere'), ('production_identity_checked',False),
                          ('installed_model_reference',{'prefix':'/tmp','mode':'installed-read-only'})]:
            document=self.marker();document[key]=value
            with self.subTest(key=key), self.assertRaises(r.ReleaseError):r.manifest_model_paths(document,self.release)
        with self.assertRaises(r.ReleaseError):r.manifest_model_paths({'wake_model_reference':str(self.target)},self.release)
    def test_wake_hash_and_link_are_verified_without_target_chmod_or_copy(self):
        before=self.target.stat()
        self.assertEqual(r.artifact_path(self.release,r.WAKE_MODEL),self.target)
        self.assertIn(r.WAKE_MODEL,r._rel_files(self.release))
        r._chmod_tree(self.release)
        self.assertEqual(r._path_bytes(self.link,release_root=self.release),0)
        after=self.target.stat()
        self.assertEqual((before.st_mode,before.st_ino,before.st_nlink,before.st_mtime_ns,before.st_ctime_ns),
                         (after.st_mode,after.st_ino,after.st_nlink,after.st_mtime_ns,after.st_ctime_ns))
        with self.assertRaises(r.ReleaseError):r._path_bytes(self.link)
    def test_wake_alias_outside_release_and_tampered_bytes_are_rejected(self):
        self.link.unlink();self.link.symlink_to(self.root/'current'/r.WAKE_MODEL)
        with self.assertRaises(r.ReleaseError):r._rel_files(self.release)
        self.link.unlink();self.link.symlink_to(self.target)
        self.target.write_bytes(b'bad')
        with self.assertRaises(r.ReleaseError):r._rel_files(self.release)
    def test_other_symlink_cannot_use_wake_exception(self):
        other=self.release/'unapproved';other.symlink_to(self.target)
        with self.assertRaises(r.ReleaseError):r._rel_files(self.release)
    def test_legacy_regular_wake_still_uses_no_follow_hashing(self):
        self.link.unlink();self.link.write_bytes(self.target.read_bytes())
        self.assertEqual(r.artifact_path(self.release,r.WAKE_MODEL),self.link)
        self.assertEqual(r.sha256_file(self.link),r.LEGACY_AMINA_SHA256)
    def test_actual_owner_guard_rejects_writable_target(self):
        guard=REAL_OWNER_GUARD
        self.target.chmod(0o666)
        with self.assertRaises(r.ReleaseError):guard(self.target,self.root)

if __name__ == '__main__': unittest.main(verbosity=2)
