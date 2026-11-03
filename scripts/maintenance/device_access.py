"""Load the F407 identity and retain one private key for maintenance access."""
import json
import os
from pathlib import Path, PurePosixPath
import secrets
import tempfile

if __package__:
    from .protocol import MaintenanceConfig, parse_config, parse_device_id, parse_server_config
else:
    from protocol import MaintenanceConfig, parse_config, parse_device_id, parse_server_config


def load_device_config(config_path, state_dir=Path('/var/lib/cns-rpi-maintenance'), root=Path('/')):
    raw = json.loads(Path(config_path).read_text())
    server_url, ca_file = parse_server_config(raw)
    device_id = read_bound_device(Path(root), raw.get('business_config'))
    if device_id is None:
        return None
    state = load_or_create_access(Path(state_dir), server_url, device_id)
    return MaintenanceConfig(server_url, device_id, state.device_key, ca_file)


def read_bound_device(root, business_path):
    if not isinstance(business_path, str) or not PurePosixPath(business_path).is_absolute():
        raise ValueError('absolute business_config required')
    business = root / business_path.lstrip('/')
    if not business.exists():
        return None
    business_config = json.loads(business.read_text())
    if not isinstance(business_config, dict) or not isinstance(business_config.get('identity', {}), dict):
        raise ValueError('invalid business identity configuration')
    binding_path = business_config.get('identity', {}).get('binding_file', '/var/lib/cns-rpi/device_binding.json')
    if not isinstance(binding_path, str) or not PurePosixPath(binding_path).is_absolute():
        raise ValueError('absolute binding_file required')
    binding = root / binding_path.lstrip('/')
    if not binding.exists():
        return None
    identity = json.loads(binding.read_text())
    if not isinstance(identity, dict) or type(identity.get('schema_version')) is not int or identity.get('schema_version') != 1 or identity.get('device_type') != 'cns_box':
        raise ValueError('invalid F407 binding')
    return parse_device_id(identity.get('device_id'))


def load_or_create_access(state_dir, server_url, device_id):
    state_dir.mkdir(parents=True, exist_ok=True)
    state_dir.chmod(0o700)
    path = state_dir / 'access.json'
    if path.exists():
        state = parse_config(json.loads(path.read_text()))
        if state.server_url != server_url or state.device_id != device_id:
            raise ValueError('device identity or server changed; access key retained')
        return state
    payload = {'schema_version': 1, 'server_url': server_url, 'device_id': device_id, 'device_key': secrets.token_urlsafe(32)}
    fd, temporary = tempfile.mkstemp(prefix='.access-', dir=state_dir)
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
    return MaintenanceConfig(server_url, device_id, payload['device_key'])
