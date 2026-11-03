import contextlib
import http.server
import io
import json
import os
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch

from scripts.maintenance.enrollment_client import enroll_once, main


@contextlib.contextmanager
def server():
    state = {'requests': [], 'status': 'pending', 'bad': False}
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            state['requests'].append((self.path, self.headers.get('Authorization'), body))
            reply = {'schema_version': 1, 'enrollment_id': body['enrollment_id'], 'status': state['status']}
            if state['status'] == 'approved':
                reply.update(node_id='node-001', token='node-token')
            if state.get('bad_token'):
                reply['token'] = ''
            if state.get('redirect'):
                self.send_response(302)
                self.send_header('Location', state['redirect'])
                self.end_headers()
                return
            if state['bad']:
                reply['enrollment_id'] = 'wrong'
            self.send_response(200)
            self.end_headers()
            self.wfile.write(json.dumps(reply).encode())
        def log_message(self, *_):
            pass
    httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    state['url'] = f'http://127.0.0.1:{httpd.server_port}'
    try:
        yield state
    finally:
        httpd.shutdown()
        httpd.server_close()
        thread.join()


class EnrollmentTests(unittest.TestCase):
    def fixture(self, root, url):
        config = root / 'etc/cns-rpi-enrollment.json'
        config.parent.mkdir(parents=True)
        config.write_text(json.dumps({'schema_version': 1, 'server_url': url, 'business_config': '/var/lib/cns-rpi/config.json'}))
        business = root / 'var/lib/cns-rpi/config.json'
        business.parent.mkdir(parents=True)
        business.write_text(json.dumps({'identity': {'binding_file': '/var/lib/cns-rpi/custom-binding.json'}}))
        binding = business.with_name('custom-binding.json')
        binding.write_text(json.dumps({'schema_version': 1, 'device_type': 'cns_box', 'device_id': 'DCDW:001.test'}))
        return config, binding

    def test_pending_restart_then_approval(self):
        with tempfile.TemporaryDirectory() as tmp, server() as remote:
            root = Path(tmp)
            config, _ = self.fixture(root, remote['url'])
            self.assertEqual(enroll_once(config, root), 'pending')
            saved = json.loads((root / 'var/lib/cns-rpi-maintenance/enrollment.json').read_text())
            remote['status'] = 'approved'
            self.assertEqual(enroll_once(config, root), 'approved')
            self.assertEqual(remote['requests'][0], remote['requests'][1])
            self.assertEqual(remote['requests'][0][1], 'Bearer ' + saved['claim_secret'])
            self.assertEqual(remote['requests'][0][0], '/api/maintenance/v1/enrollments')
            self.assertEqual(remote['requests'][0][2]['platform'], 'trixie-arm64')
            self.assertEqual(remote['requests'][0][2]['device_id'], 'DCDW:001.test')
            formal = root / 'etc/cns-rpi-maintenance.json'
            self.assertEqual(json.loads(formal.read_text())['node_id'], 'node-001')
            if os.name == 'posix':
                self.assertEqual(formal.stat().st_mode & 0o777, 0o600)
                self.assertEqual((root / 'var/lib/cns-rpi-maintenance').stat().st_mode & 0o777, 0o700)
                self.assertEqual((root / 'var/lib/cns-rpi-maintenance/enrollment.json').stat().st_mode & 0o777, 0o600)
            self.assertEqual(enroll_once(config, root), 'configured')
            self.assertEqual(len(remote['requests']), 2)

    def test_waiting_binding_and_bad_response(self):
        with tempfile.TemporaryDirectory() as tmp, server() as remote:
            root = Path(tmp)
            config, binding = self.fixture(root, remote['url'])
            original = binding.read_text()
            binding.unlink()
            self.assertEqual(enroll_once(config, root), 'waiting_identity')
            self.assertEqual(remote['requests'], [])
            binding.write_text(original)
            remote.update(status='approved', bad=True)
            with self.assertRaises(ValueError):
                enroll_once(config, root)
            self.assertFalse((root / 'etc/cns-rpi-maintenance.json').exists())

    def test_invalid_approval_redirect_and_remote_http(self):
        with tempfile.TemporaryDirectory() as tmp, server() as remote:
            root = Path(tmp)
            config, _ = self.fixture(root, remote['url'])
            remote.update(status='approved', bad_token=True)
            with self.assertRaises(ValueError):
                enroll_once(config, root)
            remote.update(bad_token=False, redirect=remote['url'] + '/elsewhere')
            with self.assertRaises(ValueError):
                enroll_once(config, root)
            self.assertEqual(len(remote['requests']), 2)
            self.assertFalse((root / 'etc/cns-rpi-maintenance.json').exists())
            raw = json.loads(config.read_text())
            raw['server_url'] = 'http://example.com'
            config.write_text(json.dumps(raw))
            with self.assertRaises(ValueError):
                enroll_once(config, root)
            self.assertEqual(len(remote['requests']), 2)

    def test_cli_output_does_not_disclose_credentials(self):
        with tempfile.TemporaryDirectory() as tmp, server() as remote:
            root = Path(tmp)
            config, _ = self.fixture(root, remote['url'])
            remote['status'] = 'approved'
            output = io.StringIO()
            actual_enroll = enroll_once
            with patch('sys.argv', ['enrollment_client.py', '--config', str(config)]), patch('scripts.maintenance.enrollment_client.os.geteuid', return_value=0, create=True), patch('scripts.maintenance.enrollment_client.enroll_once', side_effect=lambda path: actual_enroll(path, root)), contextlib.redirect_stdout(output):
                self.assertEqual(main(), 0)
            claim = json.loads((root / 'var/lib/cns-rpi-maintenance/enrollment.json').read_text())
            self.assertNotIn(claim['claim_secret'], output.getvalue())
            self.assertNotIn('node-token', output.getvalue())
            self.assertEqual(output.getvalue(), 'Enrollment: approved\n')

    def test_changed_identity_or_origin_rejected(self):
        with tempfile.TemporaryDirectory() as tmp, server() as remote:
            root = Path(tmp)
            config, binding = self.fixture(root, remote['url'])
            enroll_once(config, root)
            original = binding.read_text()
            binding.write_text(json.dumps({'schema_version': 1, 'device_type': 'cns_box', 'device_id': 'another'}))
            with self.assertRaises(ValueError):
                enroll_once(config, root)
            binding.write_text(original)
            raw = json.loads(config.read_text())
            raw['server_url'] = 'https://other.example.com'
            config.write_text(json.dumps(raw))
            with self.assertRaises(ValueError):
                enroll_once(config, root)
            self.assertEqual(len(remote['requests']), 1)

    def test_rejected_and_invalid_binding(self):
        with tempfile.TemporaryDirectory() as tmp, server() as remote:
            root = Path(tmp)
            config, binding = self.fixture(root, remote['url'])
            remote['status'] = 'rejected'
            self.assertEqual(enroll_once(config, root), 'rejected')
            binding.write_text(json.dumps({'schema_version': 1, 'device_type': 'cns_box', 'device_id': 'invalid/id'}))
            with self.assertRaises(ValueError):
                enroll_once(config, root)
            self.assertFalse((root / 'etc/cns-rpi-maintenance.json').exists())


if __name__ == '__main__':
    unittest.main()
