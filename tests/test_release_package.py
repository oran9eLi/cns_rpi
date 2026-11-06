import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('packager', ROOT / 'scripts/build_release_package.py')
packager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packager)

class ReleasePackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'source'
        self.build = self.root / 'build'
        self.source.mkdir(); self.build.mkdir()
        (self.source / 'CMakeLists.txt').write_text('project(cns_rpi VERSION 1.0.5 LANGUAGES CXX)')
        for path in packager.SOURCE_FILES:
            target = self.source / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text('fixture')
        (self.source / 'config').mkdir()
        (self.source / 'config/token.json').write_text('secret')
        for name in ('cns_rpi', 'cns_control_test'):
            data = bytearray(64); data[:7] = b'\x7fELF\x02\x01\x01'
            struct.pack_into('<HHI', data, 16, 3, 183, 1)
            struct.pack_into('<H', data, 52, 64)
            (self.build / name).write_bytes(data)
    def package(self, **kwargs):
        return packager.build_package(self.source, self.build, self.root / 'out', '1.0.5', 'a' * 40, **kwargs)
    def test_real_archive_has_hashes_modes_and_allowlist(self):
        archive = self.package()
        with tarfile.open(archive) as tar:
            manifest = json.load(tar.extractfile('manifest.json'))
            self.assertEqual(manifest['platform'], 'trixie-arm64')
            self.assertEqual(manifest['git_commit'], 'a' * 40)
            self.assertIn('ffmpeg', manifest['runtime_dependencies'])
            self.assertEqual(set(tar.getnames()), set(manifest['files']) | {'manifest.json'})
            for name, digest in manifest['files'].items():
                self.assertEqual(hashlib.sha256(tar.extractfile(name).read()).hexdigest(), digest)
                self.assertNotIn('token', name)
                self.assertNotIn('tests/', name)
                self.assertEqual(tar.getmember(name).mode, 0o644 if '/systemd/' in name else 0o755)
        self.assertEqual((archive.with_name(archive.name + '.sha256')).read_text().split()[0], hashlib.sha256(archive.read_bytes()).hexdigest())
    def test_rejects_wrong_version_platform_and_elf(self):
        for changes in ({'version':'1.0.6'}, {'platform':'bookworm-arm64'}, {'git_commit':'abc'}):
            args = dict(source=self.source, build=self.build, output=self.root/'out', version='1.0.5', git_commit='a'*40)
            args.update(changes)
            with self.assertRaises(ValueError): packager.build_package(**args)
        for offset, value in ((4, 1), (5, 2), (18, 62)):
            path = self.build/'cns_rpi'; original = path.read_bytes(); data=bytearray(original); data[offset]=value; path.write_bytes(data)
            with self.assertRaises(ValueError): self.package()
            path.write_bytes(original)

if __name__ == '__main__': unittest.main()
