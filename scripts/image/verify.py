#!/usr/bin/env python3
"""Read back a mounted factory image without starting its services."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
from urllib.parse import urlsplit


def verify_rootfs(root, source_commit):
    root = Path(root)
    for name in ('etc/cns-rpi-enrollment.json', 'boot/firmware/cns-provision.json',
                 'var/lib/cns-rpi-maintenance/access.json',
                 'var/lib/cns-rpi-maintenance/enrollment.json',
                 'var/lib/cns-rpi/device_binding.json', 'var/lib/cns-rpi-maintenance/provisioned.json'):
        if (root / name).exists():
            raise ValueError('factory image contains field identity')
    current = root / 'opt/cns-rpi/current'
    manifest = json.loads((current.resolve().parent / 'manifest.json').read_text())
    if manifest['git_commit'] != source_commit or manifest['platform'] != 'trixie-arm64':
        raise ValueError('installed release does not match source')
    maintenance_path = root / 'etc/cns-rpi-maintenance.json'
    maintenance = json.loads(maintenance_path.read_text())
    if set(maintenance) != {'schema_version', 'server_url', 'business_config'}:
        raise ValueError('maintenance configuration contains unexpected credentials')
    server = urlsplit(maintenance['server_url'])
    if (maintenance['schema_version'] != 1
            or maintenance['business_config'] != '/var/lib/cns-rpi/config.json'
            or server.scheme != 'https' or not server.hostname
            or server.username or server.password or server.query or server.fragment):
        raise ValueError('maintenance configuration values mismatch')
    if maintenance_path.stat().st_mode & 0o777 != 0o644:
        raise ValueError('maintenance configuration mode mismatch')
    config = root / 'var/lib/cns-rpi/config.json'
    business = json.loads(config.read_text())
    binding = business.get('identity', {}).get('binding_file')
    if binding and (root / binding.lstrip('/')).exists():
        raise ValueError('factory image contains field identity')
    account = next(line.split(':') for line in (root / 'etc/passwd').read_text().splitlines()
                   if line.startswith('dcdw:'))
    if config.stat().st_mode & 0o777 != 0o600 or config.stat().st_uid != int(account[2]):
        raise ValueError('business configuration ownership/mode mismatch')
    password = next(line.split(':')[1] for line in (root / 'etc/shadow').read_text().splitlines()
                    if line.startswith('dcdw:'))
    if not password.startswith(('!', '*')):
        raise ValueError('factory account must have a locked password')
    for line in (root / 'etc/group').read_text().splitlines():
        fields = line.split(':')
        if fields[0] == 'sudo' and 'dcdw' in fields[3].split(','):
            raise ValueError('factory account must not have sudo membership')
    machine_id = root / 'etc/machine-id'
    if machine_id.exists() and machine_id.read_text().strip():
        raise ValueError('factory image contains a machine identity')
    if any((root / 'etc/ssh').glob('ssh_host_*')):
        raise ValueError('factory image contains SSH host keys')
    for link in ('opt/cns-rpi/current', 'opt/cns-rpi/cellular-current',
                 'opt/cns-rpi/maintenance-current', 'usr/local/libexec/cns-rpi-apply-config'):
        path = root / link
        if not path.is_symlink() or path.readlink().is_absolute() or not path.exists():
            raise ValueError('factory release links must resolve inside the image')
    return {'source_commit': source_commit, 'release_version': manifest['version'],
            'platform': manifest['platform'], 'hardware': 'raspberry-pi-5',
            'server_url': maintenance['server_url'],
            'business_config_sha256': hashlib.sha256(config.read_bytes()).hexdigest(),
            'checks': ['release_source', 'no_field_identity', 'locked_account',
                       'no_sudo_membership', 'relative_release_links', 'config_permissions']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--source-commit', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.write_text(json.dumps(verify_rootfs(args.root, args.source_commit), indent=2) + '\n')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, StopIteration):
        sys.exit('Factory image readback failed; inspect the offline image contents.')
