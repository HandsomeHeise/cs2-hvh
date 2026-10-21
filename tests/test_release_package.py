"""Verify release contents, checksums and rejection of incomplete inputs."""
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from package_release import package


class ReleasePackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.dist = self.root / 'dist'
        (self.dist / 'licenses').mkdir(parents=True)
        for name in ('hvh_menu.dll', 'hvh_loader.exe', 'IMGUI-LICENSE.txt',
                     'MINHOOK-LICENSE.txt', 'licenses/LLVM-MinGW-LICENSE.txt',
                     'licenses/COPYING.MINGW-w64.txt'):
            (self.dist / name).write_bytes(name.encode())
        for name in ('hvh-menu.ini', 'hvh-menu.log', 'feature_tests.exe', 'game.dump'):
            (self.dist / name).write_text('must not ship')

    def make_package(self, **overrides):
        args = dict(distribution=self.dist, output=self.root / 'release',
                    version='v0.1.0', commit='a' * 40)
        args.update(overrides)
        return package(**args)

    def test_allowlist_and_checksums(self):
        archive = self.make_package()
        expected = {'hvh_menu.dll', 'hvh_loader.exe', 'IMGUI-LICENSE.txt',
                    'MINHOOK-LICENSE.txt', 'licenses/LLVM-MinGW-LICENSE.txt',
                    'licenses/COPYING.MINGW-w64.txt', 'README.md', 'TESTEN.txt',
                    'THIRD_PARTY.txt', 'BUILD-INFO.json', 'SHA256SUMS.txt'}
        with zipfile.ZipFile(archive) as source:
            self.assertEqual(set(source.namelist()), expected)
            for line in source.read('SHA256SUMS.txt').decode().splitlines():
                digest, name = line.split('  ', 1)
                self.assertEqual(digest, hashlib.sha256(source.read(name)).hexdigest())
        self.assertEqual(archive.with_suffix('.zip.sha256').read_text().split()[0],
                         hashlib.sha256(archive.read_bytes()).hexdigest())

    def test_missing_binary_fails(self):
        (self.dist / 'hvh_menu.dll').unlink()
        with self.assertRaises(FileNotFoundError):
            self.make_package()

    def test_missing_runtime_notices_fails(self):
        (self.dist / 'licenses/COPYING.MINGW-w64.txt').unlink()
        with self.assertRaises(FileNotFoundError):
            self.make_package()

    def test_invalid_metadata_fails(self):
        for overrides in ({'version': '../outside'}, {'version': 'bad name'},
                          {'commit': 'short'}, {'commit': 'z' * 40}):
            with self.subTest(overrides=overrides), self.assertRaises(ValueError):
                self.make_package(**overrides)


if __name__ == '__main__':
    unittest.main()
