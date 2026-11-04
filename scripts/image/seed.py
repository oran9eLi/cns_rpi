#!/usr/bin/env python3
"""Prepare build inputs and install a verified release into an offline rootfs."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
from types import SimpleNamespace
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/maintenance'))
from install_release import Installer, Systemd

RETIRED_NETWORK_UNITS = ('systemd-networkd.service', 'systemd-networkd.socket',
                         'systemd-networkd-wait-online.service', 'iwd.service',
                         'ModemManager.service')


class OfflineSystemd(Systemd):
    def __init__(self, root):
        self.root = root

    def _run(self, *args):
        return subprocess.run(['systemctl', '--root', str(self.root), *args],
                              check=True, capture_output=True, text=True)

    def reload(self):
        pass


def write_config(archive, sha256, source_commit, output, business_config, server_url):
    archive = Path(archive).resolve()
    with archive.open('rb') as source:
        if hashlib.file_digest(source, 'sha256').hexdigest() != sha256:
            raise ValueError('release checksum mismatch')
    with tarfile.open(archive) as tar:
        manifest = json.load(tar.extractfile('manifest.json'))
    if (manifest.get('schema_version') != 1 or manifest.get('platform') != 'trixie-arm64'
            or manifest.get('git_commit') != source_commit
            or not re.fullmatch('[0-9a-f]{40}', source_commit)):
        raise ValueError('release source/platform mismatch')
    dependencies = manifest.get('runtime_dependencies')
    if not isinstance(dependencies, list) or not all(
            isinstance(name, str) and re.fullmatch('[a-z0-9][a-z0-9+.-]*', name)
            for name in dependencies):
        raise ValueError('invalid runtime dependencies')
    maintenance_config(server_url)
    business_config = Path(business_config).resolve(strict=True)
    config = {
        'include': {'file': 'cns-rpi.yaml'},
        'packages': dependencies,
        'artefact': {'version': source_commit},
        'env': {'CNS_RELEASE_ARCHIVE': str(archive), 'CNS_RELEASE_SHA256': sha256,
                'CNS_SOURCE_COMMIT': source_commit, 'CNS_BUSINESS_CONFIG': str(business_config),
                'CNS_MAINTENANCE_SERVER_URL': server_url},
    }
    Path(output).parent.mkdir(parents=True, exist_ok=True)
    Path(output).write_text(json.dumps(config, indent=2) + '\n')


def seed(root, archive, sha256, source_commit, business_config, server_url):
    root = Path(root).resolve()
    if root == Path('/') or not root.is_dir():
        raise ValueError('an existing offline rootfs is required')
    with tarfile.open(archive) as tar:
        manifest = json.load(tar.extractfile('manifest.json'))
    services = OfflineSystemd(root)
    installer = Installer(root, services)
    artifact = SimpleNamespace(version=manifest['version'], git_commit=source_commit,
                               platform='trixie-arm64', sha256=sha256)
    release = installer.stage(archive, artifact)
    installer.prepare(release)
    config = root / 'var/lib/cns-rpi/config.json'
    config.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(business_config, config)
    config.chmod(0o600)
    account = next(line.split(':') for line in (root / 'etc/passwd').read_text().splitlines()
                   if line.startswith('dcdw:'))
    for path in (config.parent, config):
        shutil.chown(path, int(account[2]), int(account[3]))
    subprocess.run(['chroot', str(root), '/opt/cns-rpi/current/bin/cns_rpi',
                    '--check-config', '/var/lib/cns-rpi/config.json'],
                   check=True, capture_output=True, text=True)
    subprocess.run(['chroot', str(root), '/usr/bin/python3', '-c',
                    "import sys; sys.path.insert(0, '/opt/cns-rpi/cellular-current/scripts'); "
                    "from cellular_dialup import load_cellular_config; "
                    "load_cellular_config('/var/lib/cns-rpi/config.json')"],
                   check=True, capture_output=True, text=True)
    maintenance = root / 'etc/cns-rpi-maintenance.json'
    maintenance.write_text(json.dumps(maintenance_config(server_url), indent=2) + '\n')
    maintenance.chmod(0o644)
    business = json.loads(config.read_text())
    interface = business['cellular']['interface_name']
    profile = root / 'etc/NetworkManager/system-connections/cns-cellular.nmconnection'
    profile.parent.mkdir(parents=True, exist_ok=True)
    profile.write_text('[connection]\nid=cns-cellular\ntype=ethernet\n'
                       f'interface-name={interface}\nautoconnect=true\n\n'
                       '[ethernet]\n\n[ipv4]\nmethod=auto\n\n[ipv6]\nmethod=auto\n')
    profile.chmod(0o600)
    for name in ('01-eth0.network', '02-wlan0.network'):
        (root / 'etc/systemd/network' / name).unlink(missing_ok=True)
    services._run('mask', *RETIRED_NETWORK_UNITS)
    services.enable('NetworkManager.service')
    (root / 'etc/machine-id').write_text('')
    (root / 'var/lib/dbus/machine-id').unlink(missing_ok=True)
    (root / 'var/lib/systemd/random-seed').unlink(missing_ok=True)
    for key in (root / 'etc/ssh').glob('ssh_host_*'):
        key.unlink()
    return release


def maintenance_config(server_url):
    url = urlsplit(server_url)
    if (url.scheme != 'https' or not url.hostname or url.username or url.password
            or url.query or url.fragment):
        raise ValueError('an HTTPS maintenance server URL without credentials is required')
    return {'schema_version': 1, 'server_url': server_url,
            'business_config': '/var/lib/cns-rpi/config.json'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('config', 'seed'))
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--sha256', required=True)
    parser.add_argument('--source-commit', required=True)
    parser.add_argument('--business-config', type=Path, default=ROOT / 'config/config.example.json')
    parser.add_argument('--server-url', required=True)
    parser.add_argument('--root', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.action == 'config':
        if args.output is None:
            parser.error('config requires --output')
        write_config(args.archive, args.sha256, args.source_commit, args.output,
                     args.business_config, args.server_url)
    else:
        if args.root is None:
            parser.error('seed requires --root')
        seed(args.root, args.archive, args.sha256, args.source_commit,
             args.business_config, args.server_url)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, tarfile.TarError, subprocess.CalledProcessError):
        sys.exit('Factory image preparation failed; verify the release and offline rootfs.')
