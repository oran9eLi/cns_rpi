#!/usr/bin/env python3
"""Verified release staging and component-local activation."""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tarfile
import tempfile
import time
UNITS = {'app': 'cns-rpi.service', 'cellular': 'cellular-dialup.service'}
LINKS = {'app': 'current', 'cellular': 'cellular-current', 'maintenance': 'maintenance-current'}
REQUIRED = {
    'app/bin/cns_rpi',
    'app/bin/cns_control_test',
    'app/scripts/cns-rpi-apply-config.py',
    'app/scripts/migrate_telemetry_config.py',
    'app/systemd/cns-rpi.service',
    'cellular/scripts/cellular_dialup.py',
    'cellular/scripts/cellular_link.py',
    'cellular/systemd/cellular-dialup.service',
    'maintenance/maintenance_client.py',
    'maintenance/install_release.py',
    'maintenance/systemd/cns-rpi-maintenance.service',
    'maintenance/systemd/cns-rpi-maintenance.timer',
}

class Systemd:

    def check_binary(self, path, runtime=False):
        with path.open('rb') as executable:
            header = executable.read(20)
        if len(header) != 20 or header[:6] != b'\x7fELF\x02\x01' or int.from_bytes(header[18:20], 'little') != 183:
            raise ValueError('ARM64 ELF executable required')
        if runtime:
            result = subprocess.run(['ldd', str(path)], capture_output=True, text=True)
            if 'not found' in result.stdout or (result.returncode and 'statically linked' not in result.stdout + result.stderr):
                raise ValueError('missing executable runtime dependencies')

    def status(self, unit):
        try:
            return self._run('is-active', unit).stdout.strip()
        except subprocess.CalledProcessError as error:
            state = (error.stdout or '').strip()
            return state if state in ('inactive', 'failed') else 'unknown'
        except OSError:
            return 'unknown'

    def _run(self, *args):
        return subprocess.run(['systemctl', *args], check=True, capture_output=True, text=True)

    def stop(self, unit):
        self._run('stop', unit)

    def restart(self, unit):
        self._run('restart', unit)

    def reload(self):
        self._run('daemon-reload')

    def enable(self, unit):
        self._run('enable', unit)

    def observe(self, unit, executable, seconds, probe=None):
        deadline = time.monotonic() + seconds
        baseline = None
        connected = probe is None
        while True:
            try:
                values = self._run('show', unit, '--property=ActiveState,NRestarts,MainPID').stdout
                props = dict((line.split('=', 1) for line in values.splitlines()))
                if props['ActiveState'] != 'active':
                    return False
                restarts = props['NRestarts']
                if baseline is not None and restarts != baseline:
                    return False
                baseline = restarts
                if executable is not None and Path('/proc', props['MainPID'], 'exe').resolve() != executable.resolve():
                    return False
                if probe and (not connected):
                    try:
                        connected = bool(probe())
                    except (OSError, ValueError):
                        pass
            except (subprocess.CalledProcessError, OSError, KeyError):
                return False
            if time.monotonic() >= deadline:
                return connected
            time.sleep(min(2, max(0, deadline - time.monotonic())))

class Installer:

    def __init__(self, root=Path('/'), services=None):
        self.root = Path(root)
        self.home = self.root / 'opt/cns-rpi'
        self.services = services or Systemd()
        self.config = self.root / 'var/lib/cns-rpi/config.json'
        self.state = self.root / 'var/lib/cns-rpi-maintenance'

    def stage(self, archive, artifact):
        archive = Path(archive)
        with archive.open('rb') as source:
            if hashlib.file_digest(source, 'sha256').hexdigest() != artifact.sha256:
                raise ValueError('archive checksum mismatch')
        releases = self.home / 'releases'
        releases.mkdir(parents=True, exist_ok=True)
        for directory in (self.root/'opt', self.home, releases):
            directory.chmod(0o755)
        with tempfile.TemporaryDirectory(dir=releases, prefix='.staging-') as tmp:
            target = Path(tmp)
            with tarfile.open(archive, 'r:*') as tar:
                members = tar.getmembers()
                seen = set()
                for m in members:
                    p = PurePosixPath(m.name)
                    if p.is_absolute() or '..' in p.parts or '\\' in m.name or (m.name in seen) or (not (m.isfile() or m.isdir())):
                        raise ValueError('unsafe archive member')
                    seen.add(m.name)
                for m in members:
                    dest = target / m.name
                    if m.isdir():
                        dest.mkdir(parents=True, exist_ok=True)
                    else:
                        dest.parent.mkdir(parents=True, exist_ok=True)
                        with tar.extractfile(m) as src, dest.open('wb') as out:
                            shutil.copyfileobj(src, out)
                        dest.chmod(0o755 if m.mode & 0o111 else 0o644)
            manifest = json.loads((target / 'manifest.json').read_text())
            if manifest.get('schema_version') != 1 or manifest.get('version') != artifact.version or manifest.get('git_commit') != artifact.git_commit or (manifest.get('platform') != 'trixie-arm64') or (artifact.platform != 'trixie-arm64'):
                raise ValueError('incompatible manifest')
            if not re.fullmatch('[A-Za-z0-9][A-Za-z0-9._-]{0,127}', artifact.version) or not re.fullmatch('[0-9a-f]{40}', artifact.git_commit):
                raise ValueError('invalid release identity')
            files = manifest.get('files')
            actual = {p.relative_to(target).as_posix() for p in target.rglob('*') if p.is_file()} - {'manifest.json'}
            if not isinstance(files, dict) or set(files) != actual or (not REQUIRED <= actual):
                raise ValueError('incomplete manifest')
            for name, digest in files.items():
                with (target / name).open('rb') as source:
                    if hashlib.file_digest(source, 'sha256').hexdigest() != digest:
                        raise ValueError('file checksum mismatch')
            for name in ('cns_rpi', 'cns_control_test'):
                self.services.check_binary(target / 'app/bin' / name)
            deps = manifest.get('runtime_dependencies')
            if not isinstance(deps, list) or not all((isinstance(d, str) and re.fullmatch('[a-z0-9][a-z0-9+.-]*', d) for d in deps)):
                raise ValueError('invalid runtime dependencies')
            destination = releases / (artifact.version + '-' + artifact.sha256[:16])
            published = destination if destination.exists() else target
            for directory in published.rglob('*'):
                if directory.is_dir():
                    directory.chmod(0o755)
            published.chmod(0o755)
            if destination.exists():
                return destination
            os.replace(target, destination)
            return destination

    def _link(self, name, target):
        link = self.home / name
        temp = self.home / (name + '.tmp')
        temp.unlink(missing_ok=True)
        temp.symlink_to(os.path.relpath(target, self.home))
        os.replace(temp, link)

    def _version(self, path):
        return json.loads((path.parent / 'manifest.json').read_text())['version'] if path and path.exists() else None

    def status(self):
        components = {}
        for component, link in LINKS.items():
            current = self.home / link
            previous = self.home / (component + '-previous')
            manifest = json.loads((current.resolve().parent / 'manifest.json').read_text()) if current.exists() else {}
            components[component] = {'current_version': manifest.get('version'), 'git_commit': manifest.get('git_commit'), 'previous_version': self._version(previous.resolve()) if previous.exists() else None}
        return {'components': components, 'services': {unit: self.services.status(unit) if hasattr(self.services, 'status') else 'unknown' for unit in UNITS.values()}}

    def _result(self, status, component, error=None):
        return {'status': status, 'component': component, **self.status()['components'][component], 'error': error}

    def _candidate(self, release, source, component):
        candidate = self.config.parent / '.candidate-config.json'
        candidate.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, candidate)
        try:
            if component == 'app':
                self.services.check_binary(release / 'app/bin/cns_rpi', runtime=True)
                subprocess.run(['python3', str(release / 'app/scripts/migrate_telemetry_config.py'), str(candidate)], check=True, capture_output=True)
                subprocess.run([str(release / 'app/bin/cns_rpi'), '--check-config', str(candidate)], check=True, capture_output=True)
                cellular = self.home / 'cellular-current'
                if cellular.exists():
                    self._validate_cellular(cellular.resolve(), candidate)
            else:
                self._validate_cellular(release / 'cellular', candidate)
                app = self.home / 'current'
                if app.exists():
                    subprocess.run([str(app.resolve() / 'bin/cns_rpi'), '--check-config', str(candidate)], check=True, capture_output=True)
            return candidate
        except Exception:
            candidate.unlink(missing_ok=True)
            raise

    def _validate_cellular(self, cellular, candidate):
        subprocess.run(['python3', '-c', 'import sys; sys.path.insert(0,sys.argv[1]); from cellular_dialup import load_cellular_config; load_cellular_config(sys.argv[2])', str(cellular / 'scripts'), str(candidate)], check=True, capture_output=True)

    def _units(self, release, components):
        directory = self.root / 'etc/systemd/system'
        directory.mkdir(parents=True, exist_ok=True)
        for component in components:
            names = [UNITS[component]] if component != 'maintenance' else ['cns-rpi-maintenance.service', 'cns-rpi-maintenance.timer']
            for name in names:
                shutil.copy2(release / component / 'systemd' / name, directory / name)
        self.services.reload()

    def bootstrap(self, release, business_config, maintenance_config):
        dest = self.root / 'etc/cns-rpi-maintenance.json'
        if dest.exists() and json.loads(dest.read_text()) != json.loads(Path(maintenance_config).read_text()):
            raise ValueError('maintenance configuration differs; change it explicitly in a maintenance window')
        source = self.config if self.config.exists() else business_config
        candidate = self._candidate(release, source, 'app')
        self._validate_cellular(release / 'cellular', candidate)
        os.replace(candidate, self.config)
        self.config.chmod(0o600)
        dest.parent.mkdir(parents=True, exist_ok=True)
        if not dest.exists():
            shutil.copy2(maintenance_config, dest)
        dest.chmod(0o600)
        if self.root == Path('/'):
            shutil.chown(self.config.parent, user='dcdw', group='dcdw')
            shutil.chown(self.config, user='dcdw', group='dcdw')
        for component, link in LINKS.items():
            self._link(link, release / component)
        helper = self.root / 'usr/local/libexec/cns-rpi-apply-config'
        helper.parent.mkdir(parents=True, exist_ok=True)
        helper.unlink(missing_ok=True)
        helper.symlink_to(self.home / 'current/scripts/cns-rpi-apply-config.py')
        self._units(release, LINKS)
        for unit in [*UNITS.values(), 'cns-rpi-maintenance.timer']:
            self.services.enable(unit)
            self.services.restart(unit)
        if not self.services.observe(UNITS['app'], release / 'app/bin/cns_rpi', 40):
            return self._result('needs_attention', 'app', 'initial application observation failed')
        return self._result('succeeded', 'app')

    def apply(self, release, component, network_probe):
        if component not in UNITS:
            raise ValueError('unsupported component')
        return self._activate(Path(release), component, network_probe)

    def rollback(self, component, network_probe):
        if component not in UNITS:
            raise ValueError('unsupported component')
        previous = self.home / (component + '-previous')
        if not previous.exists():
            return self._result('failed', component, 'no previous release')
        backup = self.state / (component + '-config.previous')
        return self._activate(previous.resolve().parent, component, network_probe, backup if component == 'app' else self.config)

    def _activate(self, release, component, probe, source=None):
        old = (self.home / LINKS[component]).resolve()
        if old == (release / component).resolve():
            return self._result('succeeded', component)
        try:
            candidate = self._candidate(release, source or self.config, component)
        except (OSError, ValueError, subprocess.CalledProcessError):
            return self._result('failed', component, 'configuration incompatible')
        snapshot = self.config.read_bytes()
        unit = UNITS[component]
        self.state.mkdir(parents=True, exist_ok=True, mode=0o700)
        backup = self.state / (component + '-config.previous')
        pending = self.state / (component + '-config.pending')
        # Keep the last successful rollback snapshot until this activation
        # passes observation; pending also survives an interrupted activation.
        pending.write_bytes(snapshot)
        pending.chmod(0o600)
        try:
            self.services.stop(unit)
        except Exception:
            candidate.unlink(missing_ok=True)
            pending.unlink(missing_ok=True)
            return self._result('failed', component, 'service stop failed before activation')
        try:
            self._link(LINKS[component], release / component)
            if component == 'app':
                os.replace(candidate, self.config)
                if self.root == Path('/'):
                    shutil.chown(self.config, user='dcdw', group='dcdw')
            else:
                candidate.unlink()
            self._units(release, [component])
            self.services.restart(unit)
            if not self.services.observe(unit, release / 'app/bin/cns_rpi' if component == 'app' else None, 40 if component == 'app' else 180, probe if component == 'cellular' else None):
                raise RuntimeError('observation failed')
            self._link(component + '-previous', old)
            os.replace(pending, backup)
            return self._result('succeeded', component)
        except Exception:
            try:
                self.services.stop(unit)
            except Exception:
                try:
                    stopped = self.services.status(unit) in ('inactive', 'failed')
                except Exception:
                    stopped = False
                if not stopped:
                    return self._result('needs_attention', component, 'candidate could not be stopped; previous release not restored')
            try:
                self._link(LINKS[component], old)
                if component == 'app':
                    self.config.write_bytes(snapshot)
                    self.config.chmod(0o600)
                    if self.root == Path('/'):
                        shutil.chown(self.config, user='dcdw', group='dcdw')
                self._units(old.parent, [component])
                self.services.restart(unit)
                restored = self.services.observe(unit, old / 'bin/cns_rpi' if component == 'app' else None, 40 if component == 'app' else 180, probe if component == 'cellular' else None)
            except Exception:
                restored = False
            if restored:
                pending.unlink(missing_ok=True)
            return self._result('rolled_back' if restored else 'needs_attention', component, 'activation failed; previous component restored' if restored else 'activation and recovery observation failed')
        finally:
            candidate.unlink(missing_ok=True)


def main(argv=None):
    import argparse
    from types import SimpleNamespace

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['status', 'install', 'rollback', 'bootstrap'])
    parser.add_argument('--archive', type=Path)
    parser.add_argument('--artifact', type=Path)
    parser.add_argument('--component', choices=['app', 'cellular'], default='app')
    parser.add_argument('--business-config', type=Path)
    parser.add_argument('--maintenance-config', type=Path)
    args = parser.parse_args(argv)
    installer = Installer()
    if args.action == 'status':
        result = installer.status()
    elif args.component == 'cellular' and args.action != 'bootstrap':
        parser.error('cellular updates require the maintenance client HTTPS network probe')
    elif args.action == 'rollback':
        result = installer.rollback(args.component, lambda: False)
    else:
        if args.archive is None or args.artifact is None:
            parser.error('--archive and --artifact are required')
        metadata = json.loads(args.artifact.read_text())
        artifact = SimpleNamespace(**metadata)
        release = installer.stage(args.archive, artifact)
        if args.action == 'bootstrap':
            if args.business_config is None or args.maintenance_config is None:
                parser.error('--business-config and --maintenance-config are required')
            result = installer.bootstrap(release, args.business_config, args.maintenance_config)
        else:
            result = installer.apply(release, args.component, lambda: False)
    print(json.dumps(result))
    return 0 if result.get('status', 'succeeded') == 'succeeded' else 1


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.CalledProcessError, tarfile.TarError):
        print('Release installation failed; verify package and configuration.', file=__import__('sys').stderr)
        raise SystemExit(1)
