#!/usr/bin/env python3
"""Assemble the allowlisted Debian trixie ARM64 deployment package."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import struct
import subprocess
import tarfile

SOURCE_FILES = {
    'scripts/cns-rpi-apply-config.py': 'app/scripts/cns-rpi-apply-config.py',
    'scripts/migrate_telemetry_config.py': 'app/scripts/migrate_telemetry_config.py',
    'systemd/release/cns-rpi.service': 'app/systemd/cns-rpi.service',
    'scripts/cellular_dialup.py': 'cellular/scripts/cellular_dialup.py',
    'scripts/cellular_link.py': 'cellular/scripts/cellular_link.py',
    'systemd/release/cellular-dialup.service': 'cellular/systemd/cellular-dialup.service',
    'scripts/maintenance/protocol.py': 'maintenance/protocol.py',
    'scripts/maintenance/install_release.py': 'maintenance/install_release.py',
    'scripts/maintenance/maintenance_client.py': 'maintenance/maintenance_client.py',
    'scripts/maintenance/device_access.py': 'maintenance/device_access.py',
    'systemd/release/cns-rpi-maintenance.service': 'maintenance/systemd/cns-rpi-maintenance.service',
    'systemd/release/cns-rpi-maintenance.timer': 'maintenance/systemd/cns-rpi-maintenance.timer',
    'scripts/bootstrap_pi.py': 'maintenance/bootstrap_pi.py',
}
RUNTIME_DEPENDENCIES = ['libc6', 'libstdc++6', 'libgcc-s1', 'libmosquitto1', 'python3', 'python3-serial', 'ca-certificates', 'iproute2', 'iputils-ping', 'procps', 'udev', 'network-manager']

def validate_elf(path):
    header = path.read_bytes()[:64]
    if (len(header) != 64 or header[:7] != b'\x7fELF\x02\x01\x01'
            or struct.unpack_from('<HHI', header, 16) not in ((2, 183, 1), (3, 183, 1))
            or struct.unpack_from('<H', header, 52)[0] != 64):
        raise ValueError(f'{path}: expected a little-endian ELF64 AArch64 executable')

def build_package(source, build, output, version, git_commit, platform='trixie-arm64', build_metadata=None):
    source, build, output = Path(source), Path(build), Path(output)
    match = re.search(r'project\s*\(\s*cns_rpi\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)', (source/'CMakeLists.txt').read_text(), re.I)
    if not match or version != match.group(1):
        raise ValueError('release version must exactly match CMake project version')
    if platform != 'trixie-arm64' or not re.fullmatch(r'[0-9a-fA-F]{40}', git_commit):
        raise ValueError('expected trixie-arm64 and full Git commit SHA')
    files = {target: (source/path).read_bytes() for path, target in SOURCE_FILES.items()}
    for binary in ('cns_rpi', 'cns_control_test'):
        validate_elf(build/binary)
        files['app/bin/'+binary] = (build/binary).read_bytes()
    manifest = dict(schema_version=1, version=version, git_commit=git_commit.lower(), platform=platform,
                    files={name: hashlib.sha256(data).hexdigest() for name, data in sorted(files.items())},
                    runtime_dependencies=RUNTIME_DEPENDENCIES, build=build_metadata or {})
    output.mkdir(parents=True, exist_ok=True)
    manifest_data = (json.dumps(manifest, indent=2, sort_keys=True)+'\n').encode()
    archive = output/f'cns-rpi-{version}-{platform}.tar.gz'
    with tarfile.open(archive, 'w:gz') as tar:
        for name, data in sorted({**files, 'manifest.json':manifest_data}.items()):
            info = tarfile.TarInfo(name); info.size = len(data)
            info.mode = 0o644 if name == 'manifest.json' or '/systemd/' in name else 0o755
            tar.addfile(info, io.BytesIO(data))
    (output/'manifest.json').write_bytes(manifest_data)
    (output/'bootstrap_pi.py').write_bytes((source/'scripts/bootstrap_pi.py').read_bytes())
    archive.with_name(archive.name+'.sha256').write_text(hashlib.sha256(archive.read_bytes()).hexdigest()+'  '+archive.name+'\n')
    return archive

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--platform', default='trixie-arm64')
    parser.add_argument('--git-commit')
    args = parser.parse_args()
    commit = args.git_commit or subprocess.check_output(['git','-C',str(args.source),'rev-parse','HEAD'], text=True).strip()
    metadata = {name: subprocess.check_output(command, text=True).splitlines()[0]
                for name, command in {'compiler':['c++','--version'], 'cmake':['cmake','--version']}.items()}
    print(build_package(args.source, args.build, args.output, args.version, commit, args.platform, metadata))

if __name__ == '__main__': main()
