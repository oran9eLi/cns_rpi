#!/usr/bin/env python3
"""First deployment from a verified server release."""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import ssl
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request
import shutil
import hashlib
import importlib.util
import tarfile
from types import SimpleNamespace

def check_host():
    if os.geteuid() != 0:
        raise ValueError('root is required')
    if platform.machine() not in ('aarch64', 'arm64'):
        raise ValueError('ARM64 required')
    if 'VERSION_CODENAME=trixie' not in Path('/etc/os-release').read_text():
        raise ValueError('trixie required')
    fields = subprocess.run(['findmnt', '-n', '-o', 'FSTYPE,OPTIONS', '/'], capture_output=True, text=True, check=True).stdout.split()
    if fields[0] == 'overlay' or 'rw' not in fields[1].split(','):
        raise ValueError('normal writable root required')
    if os.path.ismount('/var/lib/cns-rpi') or Path('/etc/systemd/system/cns-rpi-config.service').exists():
        raise ValueError('legacy config mount requires migration')

def install_dependencies(packages):
    if not all((isinstance(p, str) and re.fullmatch('[a-z0-9][a-z0-9+.-]*', p) for p in packages)):
        raise ValueError('invalid dependency')
    subprocess.run(['apt-get', 'update'], check=True)
    subprocess.run(['apt-get', 'install', '-y', '--no-install-recommends', *packages], check=True)

class SameOriginRedirect(urllib.request.HTTPRedirectHandler):

    def __init__(self, origin):
        self.origin = origin

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if urllib.parse.urlsplit(newurl)[:2] != self.origin:
            raise ValueError('cross-origin redirect')
        return super().redirect_request(req, fp, code, msg, headers, newurl)

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', required=True)
    parser.add_argument('--maintenance-config', type=Path, required=True)
    parser.add_argument('--business-config', type=Path, required=True)
    args = parser.parse_args(argv)
    check_host()
    config = json.loads(args.maintenance_config.read_text())
    existing = Path('/etc/cns-rpi-maintenance.json')
    if existing.exists() and json.loads(existing.read_text()) != config:
        raise ValueError('maintenance configuration differs; change it explicitly before bootstrap')
    if config.get('schema_version') != 1 or not all((isinstance(config.get(k), str) and config[k] for k in ('server_url', 'node_id', 'token'))) or (not re.fullmatch('[A-Za-z0-9_-]{1,64}', config['node_id'])):
        raise ValueError('invalid maintenance configuration')
    origin = config['server_url'].rstrip('/')
    parsed = urllib.parse.urlsplit(origin)
    if parsed.username or parsed.password or parsed.query or parsed.fragment or (parsed.scheme != 'https' and (not (parsed.scheme == 'http' and parsed.hostname in ('127.0.0.1', 'localhost', '::1')))):
        raise ValueError('HTTPS required')
    opener = urllib.request.build_opener(urllib.request.HTTPSHandler(context=ssl.create_default_context(cafile=config.get('ca_file'))), SameOriginRedirect(parsed[:2]))
    headers = {'Authorization': 'Bearer ' + config['token'], 'X-CNS-Node-ID': config['node_id']}
    endpoint = origin + '/api/maintenance/v1/releases/' + urllib.parse.quote(args.version, safe='') + '?platform=trixie-arm64'
    with opener.open(urllib.request.Request(endpoint, headers=headers), timeout=30) as response:
        raw = json.load(response)
    if not isinstance(raw, dict) or raw.get('platform') != 'trixie-arm64' or (not isinstance(raw.get('version'), str)) or (not re.fullmatch('[A-Za-z0-9][A-Za-z0-9._-]{0,127}', raw['version'])) or (not re.fullmatch('[0-9a-f]{40}', raw.get('git_commit', ''))) or (not re.fullmatch('[0-9a-f]{64}', raw.get('sha256', ''))):
        raise ValueError('invalid release metadata')
    artifact = SimpleNamespace(version=raw['version'], git_commit=raw['git_commit'], platform=raw['platform'], url=raw['artifact_url'], sha256=raw['sha256'])
    if urllib.parse.urlsplit(artifact.url)[:2] != parsed[:2]:
        raise ValueError('cross-origin artifact')
    if artifact.version != args.version:
        raise ValueError('unexpected version')
    with tempfile.TemporaryDirectory(prefix='cns-bootstrap-') as tmp:
        archive = Path(tmp) / 'release.tar.gz'
        with opener.open(urllib.request.Request(artifact.url, headers=headers), timeout=60) as response, archive.open('wb') as output:
            shutil.copyfileobj(response, output)
        with archive.open('rb') as downloaded:
            if hashlib.file_digest(downloaded, 'sha256').hexdigest() != artifact.sha256:
                raise ValueError('archive checksum mismatch')
        with tarfile.open(archive, 'r:*') as package:
            # The archive SHA authenticates this installer before executing it.
            # The installer then validates and publishes the entire archive.
            members = package.getmembers()
            names = [m.name for m in members]
            if len(set(names)) != len(names):
                raise ValueError('duplicate archive members')
            member = package.getmember('maintenance/install_release.py')
            if not member.isfile():
                raise ValueError('installer must be a regular file')
            installer_path = Path(tmp) / 'install_release.py'
            with package.extractfile(member) as source, installer_path.open('wb') as output:
                shutil.copyfileobj(source, output)
        spec = importlib.util.spec_from_file_location('cns_verified_installer', installer_path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        installer = module.Installer()
        release = installer.stage(archive, artifact)
        install_dependencies(json.loads((release / 'manifest.json').read_text())['runtime_dependencies'])
        result = installer.bootstrap(release, args.business_config, args.maintenance_config)
    print(json.dumps(result))
    return 0 if result['status'] == 'succeeded' else 1
if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, OSError, subprocess.CalledProcessError):
        print('Bootstrap failed; verify host, configuration and package.', file=sys.stderr)
        raise SystemExit(1)
