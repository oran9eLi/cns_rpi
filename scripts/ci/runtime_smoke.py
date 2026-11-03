#!/usr/bin/env python3
"""Check packaged executables in an ARM64 runtime-only trixie container."""
import argparse
import json
from pathlib import Path
import platform
import subprocess
import tarfile
import tempfile

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    args=parser.parse_args()
    if platform.machine() != 'aarch64': raise SystemExit('smoke test requires native ARM64')
    if 'VERSION_CODENAME=trixie' not in Path('/etc/os-release').read_text(): raise SystemExit('smoke test requires trixie')
    for compiler in ('/usr/bin/g++','/usr/bin/gcc','/usr/bin/cmake'):
        if Path(compiler).exists(): raise SystemExit('runtime smoke container contains build tools')
    with tempfile.TemporaryDirectory() as directory:
        with tarfile.open(args.archive) as tar:
            manifest=json.load(tar.extractfile('manifest.json'))
            for name in ('app/bin/cns_rpi','app/bin/cns_control_test'):
                tar.extract(name, directory, filter='data')
                binary=str(Path(directory)/name)
                linked = subprocess.run(['ldd',binary],check=True,capture_output=True,text=True)
                print(linked.stdout, end='')
                if 'not found' in linked.stdout: raise SystemExit('unresolved runtime library')
        app=str(Path(directory)/'app/bin/cns_rpi')
        version=subprocess.run([app,'--version'],check=True,capture_output=True,text=True,timeout=20)
        if manifest['version'] not in version.stdout: raise SystemExit('binary version differs from manifest')
        config=Path(__file__).resolve().parents[2]/'config/config.example.json'
        subprocess.run([app,'--check-config',str(config)],check=True,timeout=20)
        control=str(Path(directory)/'app/bin/cns_control_test')
        payload=json.dumps({'command_id':'ci-smoke', 'command':'takeoff', 'parameters':{}})
        result=subprocess.run([control,str(config),payload],check=True,capture_output=True,text=True,timeout=20)
        if json.loads(result.stdout)['status'] != 'dry_run': raise SystemExit('control CLI did not dry run')
        print('runtime smoke passed:',manifest['version'])

if __name__=='__main__': main()
