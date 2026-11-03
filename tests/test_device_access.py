import contextlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from scripts.maintenance.maintenance_client import main
from scripts.maintenance.device_access import load_device_config


class DeviceAccessTests(unittest.TestCase):
    def fixture(self, root):
        config = root / 'etc/cns-rpi-maintenance.json'
        config.parent.mkdir()
        config.write_text(json.dumps({'schema_version': 1, 'server_url': 'https://maintenance.example', 'business_config': '/var/lib/cns-rpi/config.json'}))
        business = root / 'var/lib/cns-rpi/config.json'
        business.parent.mkdir(parents=True)
        business.write_text(json.dumps({'identity': {}}))
        binding = business.with_name('device_binding.json')
        return config, binding, root / 'var/lib/cns-rpi-maintenance'

    def bind(self, path, device_id='DCDW:001.test'):
        path.write_text(json.dumps({'schema_version': 1, 'device_type': 'cns_box', 'device_id': device_id}))

    def test_wait_then_generate_and_reuse_private_key(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config, binding, state = self.fixture(root)
            self.assertIsNone(load_device_config(config, state, root))
            self.assertFalse((state / 'access.json').exists())
            self.bind(binding)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                first = load_device_config(config, state, root)
                second = load_device_config(config, state, root)
            self.assertEqual(first, second)
            self.assertEqual(first.device_id, 'DCDW:001.test')
            self.assertEqual(len(first.device_key), 43)
            self.assertNotIn(first.device_key, output.getvalue())
            if os.name == 'posix':
                self.assertEqual(state.stat().st_mode & 0o777, 0o700)
                self.assertEqual((state / 'access.json').stat().st_mode & 0o777, 0o600)

    def test_unbound_main_exits_without_polling(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config, _, state = self.fixture(root)
            actual_load = load_device_config
            with patch('sys.argv', ['maintenance_client.py', '--config', str(config)]), patch('scripts.maintenance.maintenance_client.load_device_config', side_effect=lambda path: actual_load(path, state, root)), patch('scripts.maintenance.maintenance_client.MaintenanceClient') as client:
                self.assertEqual(main(), 0)
                client.assert_not_called()
            self.assertFalse(state.exists())

    def test_changed_identity_or_server_preserves_key(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config, binding, state = self.fixture(root)
            self.bind(binding)
            load_device_config(config, state, root)
            original = (state / 'access.json').read_bytes()
            self.bind(binding, 'other')
            with self.assertRaises(ValueError):
                load_device_config(config, state, root)
            self.bind(binding)
            raw = json.loads(config.read_text())
            raw['server_url'] = 'https://other.example'
            config.write_text(json.dumps(raw))
            with self.assertRaises(ValueError):
                load_device_config(config, state, root)
            self.assertEqual((state / 'access.json').read_bytes(), original)

    def test_invalid_binding_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config, binding, state = self.fixture(root)
            for identity in ('bad/id', '.', '..', 'x' * 21):
                self.bind(binding, identity)
                with self.assertRaises(ValueError):
                    load_device_config(config, state, root)
            self.assertFalse(state.exists())

if __name__ == '__main__':
    unittest.main()
