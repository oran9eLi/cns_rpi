import hashlib
import configparser
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from scripts.build_release_package import build_package

ROOT = Path(__file__).resolve().parents[1]


class FactoryImageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.root = self.base / 'rootfs'
        self.root.mkdir()
        self.business = ROOT / 'config/config.example.json'
        build = self.base / 'build'
        build.mkdir()
        data = bytearray(64)
        data[:7] = b'\x7fELF\x02\x01\x01'
        struct.pack_into('<HHI', data, 16, 3, 183, 1)
        struct.pack_into('<H', data, 52, 64)
        for name in ('cns_rpi', 'cns_control_test'):
            (build / name).write_bytes(data)
        import re
        version = re.search(r'project\(cns_rpi VERSION ([0-9.]+)', (ROOT / 'CMakeLists.txt').read_text()).group(1)
        self.archive = build_package(ROOT, build, self.base / 'dist', version, 'a' * 40)
        self.digest = hashlib.sha256(self.archive.read_bytes()).hexdigest()

    def module(self):
        path = ROOT / 'scripts/image/seed.py'
        self.assertTrue(path.exists(), 'factory rootfs seed entrypoint is missing')
        spec = importlib.util.spec_from_file_location('image_seed', path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    @unittest.skipUnless(shutil.which('systemctl') and os.name == 'posix', 'requires Linux offline systemctl')
    def test_real_release_seeds_offline_links_and_units_without_field_identity(self):
        shutil.copytree(ROOT / 'image/rootfs-overlay', self.root, dirs_exist_ok=True)
        for path in self.root.rglob('*'):
            if path.is_file():
                path.chmod(0o644)
        unitdir = self.root / 'usr/lib/systemd/system'
        unitdir.mkdir(parents=True)
        (unitdir / 'NetworkManager.service').write_text('[Unit]\nDescription=Fixture network manager\n[Service]\nExecStart=/bin/true\n[Install]\nWantedBy=multi-user.target\n')
        network = self.root / 'etc/systemd/network'
        network.mkdir(parents=True)
        for name in ('01-eth0.network', '02-wlan0.network'):
            (network / name).write_text('[Match]\nName=*\n')
        (self.root / 'etc/passwd').write_text(f'dcdw:x:{os.getuid()}:{os.getgid()}::/home/dcdw:/bin/bash\n')
        module = self.module()
        run = subprocess.run
        def native_boundary(command, **kwargs):
            # The fixture is ELF64 ARM64, while this test also runs on x86 WSL.
            if command[0] == 'chroot':
                return subprocess.CompletedProcess(command, 0, '', '')
            return run(command, **kwargs)
        with patch.object(module.subprocess, 'run', side_effect=native_boundary):
            release = module.seed(self.root, self.archive, self.digest, 'a' * 40,
                                  self.business, 'https://maintenance.example.invalid')
        self.assertEqual((self.root / 'opt/cns-rpi/current').resolve(), release / 'app')
        self.assertEqual((self.root / 'opt/cns-rpi/maintenance-current').resolve(), release / 'maintenance')
        helper = self.root / 'usr/local/libexec/cns-rpi-apply-config'
        self.assertFalse(os.path.isabs(os.readlink(helper)))
        self.assertEqual(helper.resolve(), release / 'app/scripts/cns-rpi-apply-config.py')
        for name in ('etc/cns-rpi-maintenance.json', 'var/lib/cns-rpi/device_binding.json', 'var/lib/cns-rpi-maintenance/provisioned.json', 'boot/firmware/cns-provision.json'):
            self.assertFalse((self.root / name).exists(), name)
        config = self.root / 'var/lib/cns-rpi/config.json'
        self.assertEqual(json.loads(config.read_text()), json.loads(self.business.read_text()))
        self.assertEqual(config.stat().st_mode & 0o777, 0o600)
        self.assertEqual(config.stat().st_uid, os.getuid())
        self.assertEqual(json.loads((self.root / 'etc/cns-rpi-enrollment.json').read_text()),
                         {'schema_version': 1, 'server_url': 'https://maintenance.example.invalid',
                          'business_config': '/var/lib/cns-rpi/config.json'})
        for name in ('cns-rpi.service', 'cellular-dialup.service', 'NetworkManager.service', 'cns-rpi-enrollment.timer'):
            result = subprocess.run(['systemctl', '--root', str(self.root), 'is-enabled', name], capture_output=True, text=True)
            self.assertEqual(result.stdout.strip(), 'enabled', name + result.stderr)
        masked = subprocess.run(['systemctl', '--root', str(self.root), 'is-enabled', 'iwd.service'], capture_output=True, text=True)
        self.assertEqual(masked.stdout.strip(), 'masked')
        self.assertFalse((network / '01-eth0.network').exists())
        self.assertFalse((network / '02-wlan0.network').exists())
        profile = self.root / 'etc/NetworkManager/system-connections/cns-cellular.nmconnection'
        parsed = configparser.ConfigParser(interpolation=None)
        parsed.read(profile)
        self.assertEqual(parsed['connection']['interface-name'], 'usb0')
        self.assertEqual(parsed['ipv4']['method'], 'auto')
        self.assertEqual(parsed['ipv6']['method'], 'auto')
        self.assertEqual(profile.stat().st_mode & 0o777, 0o600)
        (self.root / 'etc/shadow').write_text('dcdw:!:1:0:99999:7:::\n')
        (self.root / 'etc/group').write_text('dcdw:x:1000:\n')
        verifier_spec = importlib.util.spec_from_file_location('image_verify', ROOT / 'scripts/image/verify.py')
        verifier = importlib.util.module_from_spec(verifier_spec)
        verifier_spec.loader.exec_module(verifier)
        record = verifier.verify_rootfs(self.root, 'a' * 40)
        self.assertEqual(record['source_commit'], 'a' * 40)
        self.assertEqual(record['hardware'], 'raspberry-pi-5')
        self.assertNotIn('business', record)

    def test_runtime_config_uses_embedded_manifest_dependencies_and_build_inputs(self):
        output = self.base / 'runtime.yaml'
        self.module().write_config(self.archive, self.digest, 'a' * 40, output,
                                  self.business, 'https://maintenance.example.invalid')
        config = json.loads(output.read_text())
        self.assertFalse(Path(config['include']['file']).is_absolute(),
                         'image-gen rejects absolute include paths')
        manifest = json.loads((self.archive.parent / 'manifest.json').read_text())
        self.assertEqual(config['packages'], manifest['runtime_dependencies'])
        self.assertEqual(config['env']['CNS_RELEASE_ARCHIVE'], str(self.archive.resolve()))
        self.assertEqual(config['env']['CNS_SOURCE_COMMIT'], 'a' * 40)
        self.assertEqual(config['env']['CNS_RELEASE_SHA256'], self.digest)

    def test_wrong_source_or_checksum_cannot_generate_build_config(self):
        module = self.module()
        output = self.base / 'runtime.yaml'
        for digest, commit in (('0' * 64, 'a' * 40), (self.digest, 'b' * 40)):
            with self.assertRaises(ValueError):
                module.write_config(self.archive, digest, commit, output,
                                    self.business, 'https://maintenance.example.invalid')
            self.assertFalse(output.exists())

    def test_seed_rejects_the_live_host_root_before_writing(self):
        with self.assertRaises(ValueError):
            self.module().seed(Path('/'), self.archive, self.digest, 'a' * 40,
                               self.business, 'https://maintenance.example.invalid')

    def test_readback_rejects_field_identity_in_the_built_filesystem(self):
        path = ROOT / 'scripts/image/verify.py'
        self.assertTrue(path.exists(), 'image readback verifier is missing')
        spec = importlib.util.spec_from_file_location('image_verify', path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        (self.root / 'etc').mkdir()
        (self.root / 'etc/cns-rpi-maintenance.json').write_text('{"token":"must-not-ship"}')
        with self.assertRaisesRegex(ValueError, 'field identity'):
            module.verify_rootfs(self.root, 'a' * 40)


if __name__ == '__main__':
    unittest.main()
