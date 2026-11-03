import hashlib
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from types import SimpleNamespace
from scripts.maintenance.install_release import Installer, Systemd
from unittest.mock import patch

class Services:

    def check_binary(self, path, runtime=False):
        pass

    def __init__(self):
        self.calls = []
        self.results = []

    def stop(self, u):
        self.calls.append(('stop', u))

    def restart(self, u):
        self.calls.append(('restart', u))

    def reload(self):
        self.calls.append(('reload',))

    def enable(self, u):
        self.calls.append(('enable', u))

    def observe(self, u, executable, seconds, probe=None):
        self.calls.append(('observe', u, seconds))
        return self.results.pop(0) if self.results else probe() if probe else True

class InstallTests(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.root = self.base / 'root'
        self.services = Services()
        self.inst = Installer(self.root, self.services)
        self.config = self.base / 'config.json'
        self.config.write_bytes(Path('config/config.example.json').read_bytes())
        self.maint = self.base / 'maintenance.json'
        self.maint.write_text('{"token":"secret"}')

    def package(self, version='v1', extra=None, platform='trixie-arm64'):
        files = {'app/bin/cns_rpi': b'#!/bin/sh\nexit 0\n', 'app/bin/cns_control_test': b'#!/bin/sh\nexit 0\n', 'app/scripts/migrate_telemetry_config.py': b'import sys\n', 'app/scripts/cns-rpi-apply-config.py': b'pass\n', 'cellular/scripts/cellular_dialup.py': b'def load_cellular_config(path): return {}\n', 'cellular/scripts/cellular_link.py': b'', 'maintenance/maintenance_client.py': b'pass\n'}
        for name in ('app/systemd/cns-rpi.service', 'cellular/systemd/cellular-dialup.service', 'maintenance/systemd/cns-rpi-maintenance.service', 'maintenance/systemd/cns-rpi-maintenance.timer'):
            files[name] = b'[Unit]\n'
        files['app/scripts/migrate_telemetry_config.py'] = Path('scripts/migrate_telemetry_config.py').read_bytes()
        files['maintenance/install_release.py'] = Path('scripts/maintenance/install_release.py').read_bytes()
        files['cellular/scripts/cellular_dialup.py'] = Path('scripts/cellular_dialup.py').read_bytes()
        files['cellular/scripts/cellular_link.py'] = Path('scripts/cellular_link.py').read_bytes()
        manifest = {'schema_version': 1, 'version': version, 'git_commit': 'a' * 40, 'platform': platform, 'runtime_dependencies': ['python3'], 'build': {}, 'files': {k: hashlib.sha256(v).hexdigest() for k, v in files.items()}}
        files['manifest.json'] = json.dumps(manifest).encode()
        if extra:
            files.update(extra)
        archive = self.base / (version + '.tar.gz')
        with tarfile.open(archive, 'w:gz') as tar:
            for name, data in files.items():
                info = tarfile.TarInfo(name)
                info.size = len(data)
                info.mode = 493
                tar.addfile(info, io.BytesIO(data))
        artifact = SimpleNamespace(version=version, git_commit='a' * 40, platform='trixie-arm64', sha256=hashlib.sha256(archive.read_bytes()).hexdigest())
        return (archive, artifact)

    def stage(self, v):
        return self.inst.stage(*self.package(v))

    def bootstrap(self):
        return self.inst.bootstrap(self.stage('v1'), self.config, self.maint)

    def test_bootstrap_preserves_existing_state(self):
        self.assertEqual(self.bootstrap()['status'], 'succeeded')
        config = self.root / 'var/lib/cns-rpi/config.json'
        preserved = json.loads(config.read_text())
        preserved['现场'] = 'persist'
        config.write_text(json.dumps(preserved))
        before = config.read_bytes()
        binding = self.root / 'var/lib/cns-rpi/bindings.json'
        binding.write_text('bound')
        self.inst.bootstrap(self.stage('v2'), self.config, self.maint)
        self.assertEqual(config.read_bytes(), before)
        self.assertEqual(binding.read_text(), 'bound')
        self.assertEqual((self.root / 'etc/cns-rpi-maintenance.json').stat().st_mode & 511, 384)

    def test_bootstrap_rejects_conflicting_maintenance_identity(self):
        self.bootstrap()
        self.maint.write_text('{"token":"replacement"}')
        self.services.calls.clear()
        with self.assertRaises(ValueError):
            self.inst.bootstrap(self.stage('v2'), self.config, self.maint)
        self.assertEqual(self.services.calls, [])

    def test_invalid_packages_do_not_stop(self):
        for extra, platform in [({'../escape': b'x'}, 'trixie-arm64'), ({'app/bin/cns_rpi': b'bad'}, 'trixie-arm64'), (None, 'bookworm-arm64')]:
            with self.assertRaises(ValueError):
                self.inst.stage(*self.package(extra=extra, platform=platform))
        archive, a = self.package()
        a.sha256 = '0' * 64
        with self.assertRaises(ValueError):
            self.inst.stage(archive, a)
        self.assertEqual(self.services.calls, [])

    def test_release_is_traversable_and_restage_keeps_running_inode(self):
        release = self.stage('v1')
        self.assertEqual(release.stat().st_mode & 511, 493)
        executable = release / 'app/bin/cns_rpi'
        inode = executable.stat().st_ino
        self.assertEqual(self.stage('v1'), release)
        self.assertEqual(executable.stat().st_ino, inode)

    def test_app_update_and_rollback_preserve_bindings(self):
        self.bootstrap()
        self.services.calls.clear()
        self.assertEqual(self.inst.apply(self.stage('v2'), 'app', lambda: True)['status'], 'succeeded')
        self.assertFalse(any(('cellular' in str(c) for c in self.services.calls)))
        self.assertEqual(self.inst.rollback('app', lambda: True)['current_version'], 'v1')

    def test_app_failure_restores_config_and_link(self):
        self.bootstrap()
        self.services.results = [False, True]
        before = (self.root / 'var/lib/cns-rpi/config.json').read_bytes()
        result = self.inst.apply(self.stage('v2'), 'app', lambda: True)
        self.assertEqual(result['status'], 'rolled_back')
        self.assertEqual(result['current_version'], 'v1')
        self.assertEqual((self.root / 'var/lib/cns-rpi/config.json').read_bytes(), before)

    def test_cellular_requires_network_and_reports_failed_restore(self):
        self.bootstrap()
        self.services.results = [False, False]
        result = self.inst.apply(self.stage('v2'), 'cellular', lambda: False)
        self.assertEqual(result['status'], 'needs_attention')
        self.assertEqual(result['current_version'], 'v1')

    def test_manual_rollback_validates_current_config(self):
        self.bootstrap()
        self.inst.apply(self.stage('v2'), 'app', lambda: True)
        current = self.root / 'opt/cns-rpi/current'
        previous = self.inst.home / 'app-previous'
        (previous.resolve() / 'bin/cns_rpi').write_text('#!/bin/sh\nexit 1\n')
        self.services.calls.clear()
        self.assertEqual(self.inst.rollback('app', lambda: True)['status'], 'failed')
        self.assertEqual(current.resolve().parent.name.split('-')[0], 'v2')
        self.assertEqual(self.services.calls, [])

    def test_app_preflight_uses_current_cellular_validator(self):
        self.bootstrap()
        current = self.root / 'opt/cns-rpi/cellular-current/scripts/cellular_dialup.py'
        current.write_text('def load_cellular_config(path): raise ValueError("incompatible")\n')
        self.services.calls.clear()
        self.assertEqual(self.inst.apply(self.stage('v2'), 'app', lambda: True)['status'], 'failed')
        self.assertEqual(self.services.calls, [])

    def test_manual_rollback_restores_previous_configuration(self):
        self.bootstrap()
        config = self.root / 'var/lib/cns-rpi/config.json'
        original = config.read_bytes()
        self.inst.apply(self.stage('v2'), 'app', lambda: True)
        changed = json.loads(config.read_text())
        changed['mqtt']['host'] = 'new-host'
        config.write_text(json.dumps(changed))
        self.assertEqual(self.inst.rollback('app', lambda: True)['status'], 'succeeded')
        self.assertEqual(config.read_bytes(), original)

    def test_bootstrap_health_failure_is_not_success(self):
        self.services.results = [False]
        self.assertEqual(self.bootstrap()['status'], 'needs_attention')

    def test_real_platform_boundary_rejects_script_and_missing_libraries(self):
        executable=self.base/'binary'
        executable.write_bytes(b'#!/bin/sh\n')
        with self.assertRaises(ValueError): Systemd().check_binary(executable)
        header=bytearray(20)
        header[:6]=b'\x7fELF\x02\x01'
        header[18:20]=(183).to_bytes(2,'little')
        executable.write_bytes(header)
        Systemd().check_binary(executable)
        with patch('scripts.maintenance.install_release.subprocess.run') as run:
            run.return_value.stdout='libmissing.so => not found'
            run.return_value.stderr=''
            run.return_value.returncode=0
            with self.assertRaises(ValueError): Systemd().check_binary(executable,runtime=True)

    def test_failed_app_attempt_does_not_overwrite_previous_rollback_config(self):
        self.bootstrap()
        original=self.inst.config.read_bytes()
        self.inst.apply(self.stage('v2'),'app',lambda:True)
        self.services.results=[False,True]
        self.inst.apply(self.stage('v3'),'app',lambda:True)
        self.assertEqual((self.inst.state/'app-config.previous').read_bytes(),original)
        self.assertEqual(self.inst.rollback('app',lambda:True)['current_version'],'v1')
