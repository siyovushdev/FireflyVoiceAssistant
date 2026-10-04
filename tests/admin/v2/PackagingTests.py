"""Build-time allowlist rejects traversal, missing/corrupt data and symlinks."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('packager', Path(__file__).resolve().parents[3] / 'scripts/package-admin-v2.py')
packager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packager)


class PackagingTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        (self.root / 'index.html').write_bytes(b'<html>offline</html>')
        (self.root / 'assets').mkdir()
        (self.root / 'assets/app-abcd1234.js').write_bytes(b'export const x=1;')
        self.entries = [{'file': name, 'sha256': hashlib.sha256((self.root / name).read_bytes()).hexdigest()}
                        for name in ['index.html', 'assets/app-abcd1234.js']]
        self.write()

    def tearDown(self):
        self.directory.cleanup()

    def write(self):
        (self.root / 'firefly-assets.json').write_text(json.dumps({'version': 1, 'files': self.entries}))

    def test_valid_embeds_only_declared_bytes(self):
        (self.root / 'secret.txt').write_text('not an asset')
        output = self.root / 'generated.h'
        packager.package(self.root, output)
        self.assertEqual(len(packager.read_assets(self.root)), 2)
        text = output.read_text()
        self.assertIn('assets.emplace("/"', text)
        self.assertNotIn('/v2/assets/', text)
        self.assertIn('/assets/app-abcd1234.js', text)
        self.assertNotIn('secret', text)
        self.assertNotIn(str(self.root), text)

    def test_traversal_and_arbitrary_types(self):
        for name in ['../index.html', '/etc/passwd', 'assets/../index.html', 'assets/a.js?x', 'assets/x.json', 'assets/x.js%00', 'assets/a\\b.js']:
            self.entries[1]['file'] = name
            self.write()
            with self.assertRaises(ValueError):
                packager.read_assets(self.root)

    def test_missing_corrupt_duplicate(self):
        path = self.root / self.entries[1]['file']
        path.write_text('tampered')
        with self.assertRaises(ValueError):
            packager.read_assets(self.root)
        path.unlink()
        with self.assertRaises(ValueError):
            packager.read_assets(self.root)
        self.entries = [self.entries[0], self.entries[0]]
        self.write()
        with self.assertRaises(ValueError):
            packager.read_assets(self.root)

    def test_symlink_file_directory_and_manifest(self):
        asset = self.root / self.entries[1]['file']
        asset.unlink()
        asset.symlink_to(self.root / 'index.html')
        with self.assertRaises(ValueError):
            packager.read_assets(self.root)
        asset.unlink()
        (self.root / 'assets').rmdir()
        (self.root / 'assets').symlink_to(self.root, target_is_directory=True)
        with self.assertRaises(ValueError):
            packager.read_assets(self.root)
        manifest = self.root / 'firefly-assets.json'
        manifest.rename(self.root / 'manifest-copy.json')
        manifest.symlink_to(self.root / 'manifest-copy.json')
        with self.assertRaises(ValueError):
            packager.read_assets(self.root)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]])
