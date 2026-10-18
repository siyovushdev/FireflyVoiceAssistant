"""CPU/private-file release admission; no services, models or target hardware."""
import copy
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from packaging import firefly_release as r
from packaging import mqtt_release as m


def candidate():
    return {"offline_candidate": {"state": "OWNER_ACTIVATION_REQUIRED", "models": "frozen-target-references"},
            "production_identity_checked": False, "runtime_prefix": r.PRODUCTION_PREFIX,
            "source": {"identity": "committed-offline-candidate", "dirty": False, "commit": "a" * 40},
            "build": {"matches_worktree": True, "dirty": False, "head": "a" * 40},
            "shared_models": m.model_references(), "mqtt": {"enabled": True}}


class MqttReleaseAdmission(unittest.TestCase):
    def test_frozen_references_are_complete_and_do_not_claim_model_bytes_checked(self):
        document = candidate()
        m.assert_candidate(document)
        self.assertEqual({x['path'] for x in document['shared_models']}, set(r.ALLOWED_SHARED_MODELS))
        self.assertEqual(sum(x['bytes'] for x in document['shared_models']), r.PRODUCTION_MODEL_BYTES)
        self.assertFalse(document['production_identity_checked'])

    def test_no_marker_tampering_promotes_an_unchecked_model_contract(self):
        original = candidate()
        cases = []
        for field, value in [('production_identity_checked', True), ('runtime_prefix', '/tmp/alias'),
                             ('installed_model_reference', {'mode': 'installed-read-only'}),
                             ('wake_model_reference', '/tmp/model'), ('offline_candidate', {}),
                             ('mqtt', {'enabled': False})]:
            doc = copy.deepcopy(original); doc[field] = value; cases.append(doc)
        for field, value in [('dirty', True), ('identity', 'committed-production'), ('commit', 'b' * 40)]:
            doc = copy.deepcopy(original); doc['source'][field] = value; cases.append(doc)
        for field, value in [('dirty', True), ('matches_worktree', False), ('head', 'short')]:
            doc = copy.deepcopy(original); doc['build'][field] = value; cases.append(doc)
        for field, value in [('sha256', '0' * 64), ('bytes', 1), ('path', '../model')]:
            doc = copy.deepcopy(original); doc['shared_models'][0][field] = value; cases.append(doc)
        for doc in cases:
            with self.subTest(doc=doc), self.assertRaises(r.ReleaseError):
                m.assert_candidate(doc)

    def test_finalization_requires_both_explicit_owner_authorization_and_root(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for accepted, uid in [(False, 0), (True, 1000), (False, 1000)]:
                with patch.object(r, '_owner_accepted', return_value=accepted), patch.object(os, 'geteuid', return_value=uid):
                    with self.assertRaisesRegex(r.ReleaseError, 'owner authorization and root'):
                        m.finalize_candidate(root / 'source', root / 'output', 'new')
                self.assertEqual(list(root.iterdir()), [])

    def test_missing_target_models_publish_no_finalized_stage(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with patch.object(r, '_owner_accepted', return_value=True), patch.object(os, 'geteuid', return_value=0), \
                 patch.object(r, 'verify_tree', return_value=candidate()), \
                 patch.object(r, 'installed_model_paths', side_effect=r.ReleaseError('missing actual target model')):
                with self.assertRaisesRegex(r.ReleaseError, 'actual target model'):
                    m.finalize_candidate(root / 'source', root / 'output', 'new')
            self.assertFalse((root / 'output').exists())
            self.assertFalse(any(root.glob('.candidate-*')))

    def test_ordinary_install_verify_and_rollback_cannot_accept_candidate_marker(self):
        # The real verifier must fail before any model/path probing, ELF execution,
        # pointer mutation or install publication. Only manifest loading is injected.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'releases/new').mkdir(parents=True)
            document = candidate()
            with patch.object(r, 'load_manifest', return_value=document), \
                 patch.object(r, 'manifest_model_paths', side_effect=AssertionError('model access before admission')):
                with self.assertRaisesRegex(r.ReleaseError, 'owner finalization'):
                    r.verify_tree(root, 'new')
                with self.assertRaisesRegex(r.ReleaseError, 'owner finalization'):
                    r.install_release(root, root / 'inactive', 'new')
            self.assertFalse((root / 'inactive').exists())
            self.assertFalse((root / 'current').exists())
            (root / 'current').symlink_to('releases/old')
            (root / 'previous-release').write_text('new\n')
            with patch.object(r, 'load_manifest', return_value=document):
                with self.assertRaisesRegex(r.ReleaseError, 'owner finalization'):
                    r.activate(root, 'new', root / 'unused-store')
                with self.assertRaisesRegex(r.ReleaseError, 'owner finalization'):
                    r.rollback(root, root / 'unused-store')
            self.assertEqual(os.readlink(root / 'current'), 'releases/old')
            self.assertEqual((root / 'previous-release').read_text(), 'new\n')


if __name__ == '__main__':
    unittest.main(verbosity=2)
