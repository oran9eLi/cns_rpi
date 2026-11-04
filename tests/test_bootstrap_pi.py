import tempfile
from pathlib import Path
import unittest
import subprocess
import shutil
from unittest.mock import patch
from scripts.bootstrap_pi import check_host, install_dependencies, SameOriginRedirect

class BootstrapTests(unittest.TestCase):

    def test_bootstrap_is_standalone_on_blank_host(self):
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / 'bootstrap_pi.py'
            shutil.copy2('scripts/bootstrap_pi.py', script)
            result = subprocess.run(['python3', str(script), '--help'], capture_output=True, text=True, cwd=directory)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_checks_happen_without_apt(self):
        with patch('scripts.bootstrap_pi.os.geteuid', return_value=1000):
            with self.assertRaises(ValueError):
                check_host()
        with patch('scripts.bootstrap_pi.os.geteuid', return_value=0), patch('scripts.bootstrap_pi.platform.machine', return_value='x86_64'):
            with self.assertRaises(ValueError):
                check_host()

    def test_dependency_names_pass_as_arguments(self):
        with patch('scripts.bootstrap_pi.subprocess.run') as run:
            install_dependencies(['python3', 'libssl3t64'])
            self.assertEqual(run.call_args_list[-1].args[0], ['apt-get', 'install', '-y', '--no-install-recommends', 'python3', 'libssl3t64'])
            with self.assertRaises(ValueError):
                install_dependencies(['bad;touch /x'])

    def test_platform_and_writable_root_precede_dependencies(self):
        with patch('scripts.bootstrap_pi.os.geteuid', return_value=0), patch('scripts.bootstrap_pi.platform.machine', return_value='aarch64'), patch('scripts.bootstrap_pi.Path.read_text', return_value='VERSION_CODENAME=bookworm'), patch('scripts.bootstrap_pi.subprocess.run') as run:
            with self.assertRaises(ValueError):
                check_host()
            run.assert_not_called()
        for filesystem in ['overlay rw,relatime', 'ext4 ro,relatime']:
            with self.subTest(filesystem=filesystem), patch('scripts.bootstrap_pi.os.geteuid', return_value=0), patch('scripts.bootstrap_pi.platform.machine', return_value='aarch64'), patch('scripts.bootstrap_pi.Path.read_text', return_value='VERSION_CODENAME=trixie'), patch('scripts.bootstrap_pi.subprocess.run') as run:
                run.return_value.stdout = filesystem
                with self.assertRaises(ValueError):
                    check_host()
                self.assertEqual(run.call_args.args[0][0], 'findmnt')

    def test_redirect_cannot_forward_bearer_to_another_origin(self):
        redirect = SameOriginRedirect(('https', 'server'))
        with self.assertRaises(ValueError):
            redirect.redirect_request(None, None, 302, '', {}, 'https://other/package')
