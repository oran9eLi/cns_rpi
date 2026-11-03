#!/usr/bin/env python3
"""Request operator-approved maintenance access after the business service learns identity."""
import argparse
import json
import os
from pathlib import Path, PurePosixPath
import re
import secrets
import ssl
import sys
import tempfile
import urllib.request
import uuid

try:
    from .protocol import parse_config
except ImportError:
    from protocol import parse_config


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise ValueError('enrollment redirect refused')


def atomic_json(path, payload):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix='.' + path.name, dir=path.parent)
    try:
        os.chmod(temporary, 0o600)
        with os.fdopen(fd, 'w') as output:
            json.dump(payload, output)
            output.write('\n')
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def enroll_once(config_path=Path('/etc/cns-rpi-enrollment.json'), root=Path('/')):
    root = Path(root)
    formal = root / 'etc/cns-rpi-maintenance.json'
    if formal.exists():
        return 'configured'
    settings, business_path = load_enrollment_config(config_path)
    device_id = read_bound_device(root, business_path)
    if device_id is None:
        return 'waiting_identity'
    state = load_or_create_claim(root, settings, device_id)
    payload = {'schema_version': 1, 'enrollment_id': state['enrollment_id'], 'device_id': device_id, 'platform': 'trixie-arm64'}
    opener = urllib.request.build_opener(NoRedirect(), urllib.request.HTTPSHandler(context=ssl.create_default_context(cafile=settings.ca_file)))
    request = urllib.request.Request(settings.server_url + '/api/maintenance/v1/enrollments', data=json.dumps(payload).encode(), headers={'Authorization': 'Bearer ' + state['claim_secret'], 'Content-Type': 'application/json'}, method='POST')
    with opener.open(request, timeout=30) as response:
        reply = json.load(response)
    approved = validate_response(reply, state['enrollment_id'], settings)
    if approved is not None:
        atomic_json(formal, approved)
    return reply['status']


def load_enrollment_config(config_path):
    raw = json.loads(Path(config_path).read_text())
    # Reuse the maintenance URL and CA boundary before any outbound request.
    settings = parse_config({'schema_version': raw.get('schema_version'), 'server_url': raw.get('server_url'), 'ca_file': raw.get('ca_file'), 'node_id': 'enrollment', 'token': 'validation'})
    business_path = raw.get('business_config')
    if not isinstance(business_path, str) or not PurePosixPath(business_path).is_absolute():
        raise ValueError('absolute business_config required')
    return settings, business_path


def read_bound_device(root, business_path):
    business = root / business_path.lstrip('/')
    if not business.exists():
        return None
    binding_path = json.loads(business.read_text()).get('identity', {}).get('binding_file', '/var/lib/cns-rpi/device_binding.json')
    if not isinstance(binding_path, str) or not PurePosixPath(binding_path).is_absolute():
        raise ValueError('absolute binding_file required')
    binding = root / binding_path.lstrip('/')
    if not binding.exists():
        return None
    identity = json.loads(binding.read_text())
    device_id = identity.get('device_id')
    if identity.get('schema_version') != 1 or identity.get('device_type') != 'cns_box' or not isinstance(device_id, str) or not re.fullmatch(r'[A-Za-z0-9_.:-]{1,20}', device_id):
        raise ValueError('invalid F407 binding')
    return device_id


def load_or_create_claim(root, settings, device_id):
    state_path = root / 'var/lib/cns-rpi-maintenance/enrollment.json'
    state_path.parent.mkdir(parents=True, exist_ok=True)
    state_path.parent.chmod(0o700)
    if state_path.exists():
        state = json.loads(state_path.read_text())
        if state.get('schema_version') != 1 or state.get('device_id') != device_id or state.get('server_url') != settings.server_url:
            raise ValueError('enrollment identity or server changed')
        if not isinstance(state.get('enrollment_id'), str) or not re.fullmatch(r'[0-9a-f]{32}', state['enrollment_id']) or not isinstance(state.get('claim_secret'), str) or not re.fullmatch(r'[A-Za-z0-9_-]{43}', state['claim_secret']):
            raise ValueError('invalid saved enrollment')
    else:
        state = {'schema_version': 1, 'server_url': settings.server_url, 'device_id': device_id, 'enrollment_id': uuid.uuid4().hex, 'claim_secret': secrets.token_urlsafe(32)}
        atomic_json(state_path, state)
    return state


def validate_response(reply, enrollment_id, settings):
    if not isinstance(reply, dict) or reply.get('schema_version') != 1 or reply.get('enrollment_id') != enrollment_id or reply.get('status') not in ('pending', 'rejected', 'approved'):
        raise ValueError('invalid enrollment response')
    if reply['status'] == 'approved':
        approved = {'schema_version': 1, 'server_url': settings.server_url, 'node_id': reply.get('node_id'), 'token': reply.get('token')}
        if settings.ca_file is not None:
            approved['ca_file'] = settings.ca_file
        parse_config(approved)
        return approved
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=Path('/etc/cns-rpi-enrollment.json'))
    args = parser.parse_args()
    if os.geteuid() != 0:
        print('Enrollment requires root.', file=sys.stderr)
        return 1
    try:
        print('Enrollment: ' + enroll_once(args.config))
        return 0
    except (ValueError, OSError, KeyError, TypeError):
        print('Enrollment failed; inspect configuration and connectivity.', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
