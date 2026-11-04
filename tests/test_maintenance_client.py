import contextlib
import hashlib
import http.server
import json
from pathlib import Path
import tempfile
import threading
import unittest

from scripts.maintenance.protocol import parse_artifact, parse_poll, parse_config
from scripts.maintenance.maintenance_client import MaintenanceClient
from scripts.maintenance.install_release import Installer
from tests import test_maintenance_install as installer_fixtures


class InstallerBoundary:
    def __init__(self):
        self.installs = []
        self.staged = []

    def status(self):
        return {"components": {"app": {"version": "1.0.5"}}, "services": {"cns-rpi.service": "active"}}

    def stage(self, archive, artifact):
        self.staged.append(archive.read_bytes())
        return archive.parent

    def apply(self, release, component, network_probe):
        self.installs.append(component)
        return {"status": "succeeded", "component": component, "current_version": "1.0.6", "previous_version": "1.0.5", "error": ""}

    def rollback(self, component, network_probe):
        self.installs.append("rollback:" + component)
        return {"status": "rolled_back", "component": component, "current_version": "1.0.5", "previous_version": "1.0.6", "error": ""}


@contextlib.contextmanager
def maintenance_server():
    state = {"requests": [], "downloads": [], "job": None, "ack": None, "artifact": b"a release fixture", "redirect": None, "drop_result": False, "access": "approved"}

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            state["requests"].append((self.path, self.headers.get("Authorization"), json.loads(self.rfile.read(int(self.headers["Content-Length"])))))
            if state["drop_result"] and (state["requests"][-1][2].get("job") or {}).get("status") == "succeeded":
                self.send_error(503)
                return
            payload = {"schema_version": 1, "access": state["access"], "acknowledged_job_id": state["ack"], "job": state["job"]}
            content = json.dumps(payload).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(content)

        def do_GET(self):
            state["downloads"].append((self.path, self.headers.get("Authorization")))
            if state["redirect"]:
                self.send_response(302)
                self.send_header("Location", state["redirect"])
                self.end_headers()
                return
            self.send_response(200)
            self.end_headers()
            self.wfile.write(state["artifact"])

        def log_message(self, *_):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    state["url"] = f"http://127.0.0.1:{server.server_port}"
    try:
        yield state
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def install_job(server, job_id="job-1"):
    return {"job_id": job_id, "action": "install", "component": "app", "version": "1.0.6", "git_commit": "a" * 40,
            "platform": "trixie-arm64", "artifact_url": server["url"] + "/release.tar.gz",
            "sha256": hashlib.sha256(server["artifact"]).hexdigest()}


class ProtocolTests(unittest.TestCase):
    def test_only_configured_origin_can_supply_packages(self):
        with maintenance_server() as server:
            raw = install_job(server)
            self.assertEqual(parse_artifact(raw, server["url"]).version, "1.0.6")
            for url in ("https://elsewhere.example/package", "https://name:password@example.com/file", "http://169.254.169.254/latest"):
                with self.subTest(url=url), self.assertRaises(ValueError):
                    parse_artifact({**raw, "artifact_url": url}, server["url"])

    def test_invalid_jobs_rejected_at_protocol_boundary(self):
        with maintenance_server() as server:
            good = install_job(server)
            for change in ({"component": "maintenance"}, {"action": "shell"}, {"job_id": "../other"}, {"sha256": "wrong"}, {"platform": "bookworm-arm64"}):
                with self.subTest(change=change), self.assertRaises(ValueError):
                    parse_poll({"schema_version": 1, "access": "approved", "job": {**good, **change}}, server["url"])
            with self.assertRaises(ValueError):
                parse_poll({"schema_version": 2, "job": None}, server["url"])

    def test_remote_plain_http_and_invalid_device_config_rejected(self):
        raw = {"schema_version": 1, "server_url": "https://maintenance.example", "device_id": "pi-001", "device_key": "secret"}
        self.assertEqual(parse_config(raw).device_id, "pi-001")
        for change in ({"server_url": "http://maintenance.example"}, {"device_id": "../other"}, {"device_id": "."}, {"device_id": ".."}, {"device_id": "x" * 21}, {"device_key": ""}, {"device_key": "bad\nkey"}, {"device_key": "bad\x7fkey"}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                parse_config({**raw, **change})


class ClientTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.state_dir = Path(self.temp.name)
        self.installer = InstallerBoundary()

    def client(self, server):
        config = parse_config({"schema_version": 1, "server_url": server["url"], "device_id": "pi-001", "device_key": "test-secret"})
        return MaintenanceClient(config, self.installer, self.state_dir)

    def state(self):
        return json.loads((self.state_dir / "job.json").read_text())

    def test_idle_poll_does_not_install(self):
        with maintenance_server() as server:
            self.client(server).run_once()
            self.assertEqual(self.installer.installs, [])
            path, auth, report = server["requests"][0]
            self.assertEqual(path, "/api/maintenance/v1/devices/pi-001/poll")
            self.assertEqual(auth, "Bearer test-secret")
            self.assertEqual(report["schema_version"], 1)
            self.assertIsNone(report["job"])

    def test_unapproved_response_never_executes_job(self):
        with maintenance_server() as server:
            for access in ('pending', 'rejected'):
                server['access'] = access
                self.client(server).run_once()
                self.assertEqual(self.installer.installs, [])
                server['job'] = install_job(server)
                with self.assertRaises(ValueError):
                    self.client(server).run_once()
                self.assertEqual(self.installer.installs, [])
                server['job'] = None

    def test_device_id_is_encoded_in_poll_path(self):
        with maintenance_server() as server:
            config = parse_config({'schema_version': 1, 'server_url': server['url'], 'device_id': 'DCDW:001.test', 'device_key': 'test-secret'})
            MaintenanceClient(config, self.installer, self.state_dir).run_once()
            self.assertEqual(server['requests'][0][0], '/api/maintenance/v1/devices/DCDW%3A001.test/poll')

    def test_install_reports_terminal_result_and_redelivery_never_reinstalls(self):
        with maintenance_server() as server:
            server["job"] = install_job(server)
            self.client(server).run_once()
            self.assertEqual(self.installer.installs, ["app"])
            self.assertEqual(self.state()["status"], "succeeded")
            self.assertEqual(server["requests"][-1][2]["job"]["status"], "succeeded")
            self.client(server).run_once()
            self.assertEqual(self.installer.installs, ["app"])
            self.assertNotIn("test-secret", json.dumps(self.state()))

    def test_new_job_waits_for_previous_result_acknowledgement(self):
        with maintenance_server() as server:
            server["job"] = install_job(server)
            self.client(server).run_once()
            server["job"] = install_job(server, "job-2")
            self.client(server).run_once()
            self.assertEqual(self.installer.installs, ["app"])
            self.assertEqual(self.state()["job_id"], "job-1")
            server["ack"] = "job-1"
            self.client(server).run_once()
            self.assertEqual(self.installer.installs, ["app", "app"])
            self.assertEqual(self.state()["job_id"], "job-2")

    def test_interrupted_task_requires_attention_instead_of_repeating_install(self):
        (self.state_dir / "job.json").write_text(json.dumps({"job_id": "job-1", "action": "install", "component": "app", "phase": "installing", "status": "running"}))
        with maintenance_server() as server:
            server["job"] = install_job(server)
            self.client(server).run_once()
            self.assertEqual(self.installer.installs, [])
            self.assertEqual(self.state()["status"], "needs_attention")
            self.assertEqual(server["requests"][0][2]["job"]["status"], "needs_attention")

    def test_bad_checksum_never_reaches_install(self):
        with maintenance_server() as server:
            helper = installer_fixtures.InstallTests()
            helper.base = self.state_dir
            archive, _ = helper.package("1.0.6")
            server["artifact"] = archive.read_bytes()
            server["job"] = {**install_job(server), "sha256": "0" * 64}
            services = installer_fixtures.Services()
            self.installer = Installer(self.state_dir / "device", services)
            self.client(server).run_once()
            self.assertEqual(services.calls, [])
            self.assertEqual(self.state()["status"], "failed")

    def test_real_installer_updates_component_and_reports_result_over_http(self):
        helper = installer_fixtures.InstallTests()
        helper.base = self.state_dir
        services = installer_fixtures.Services()
        self.installer = Installer(self.state_dir / "device", services)
        config = self.state_dir / "config.json"
        config.write_bytes(Path("config/config.example.json").read_bytes())
        maintenance = self.state_dir / "maintenance.json"
        maintenance.write_text('{"device_key":"test-secret"}')
        initial = self.installer.stage(*helper.package("1.0.5"))
        self.installer.bootstrap(initial, config, maintenance)
        binding = self.installer.config.parent / "device_binding.json"
        binding.write_text("preserved identity")
        services.calls.clear()
        with maintenance_server() as server:
            archive, _ = helper.package("1.0.6")
            server["artifact"] = archive.read_bytes()
            server["job"] = install_job(server)
            self.client(server).run_once()
            self.assertEqual(self.state()["status"], "succeeded")
            self.assertEqual(self.installer.status()["components"]["app"]["current_version"], "1.0.6")
            self.assertEqual(self.installer.status()["components"]["cellular"]["current_version"], "1.0.5")
            self.assertEqual(binding.read_text(), "preserved identity")
            self.assertFalse(any("cellular-dialup.service" in call for call in services.calls))
            self.assertEqual(server["requests"][-1][2]["job"]["current_version"], "1.0.6")
            self.assertEqual(self.state()["error"], "")

    def test_result_survives_failed_report_and_client_restart(self):
        with maintenance_server() as server:
            server["job"] = install_job(server)
            server["drop_result"] = True
            self.client(server).run_once()
            self.assertEqual(self.state()["status"], "succeeded")
            self.assertFalse(self.state()["acknowledged"])
            server["drop_result"] = False
            server["ack"] = "job-1"
            server["job"] = None
            self.client(server).run_once()
            self.assertTrue(self.state()["acknowledged"])
            self.assertEqual(self.installer.installs, ["app"])

    def test_download_redirect_is_not_followed(self):
        with maintenance_server() as server, maintenance_server() as foreign:
            server["job"] = install_job(server)
            server["redirect"] = foreign["url"] + "/leak"
            self.client(server).run_once()
            self.assertEqual(self.installer.installs, [])
            self.assertEqual(foreign["downloads"], [])
            self.assertEqual(self.state()["status"], "failed")

    def test_rollback_uses_same_authorized_task_flow(self):
        with maintenance_server() as server:
            server["job"] = {"job_id": "rollback-1", "action": "rollback", "component": "app"}
            self.client(server).run_once()
            self.assertEqual(self.installer.installs, ["rollback:app"])
            self.assertEqual(self.state()["status"], "rolled_back")


if __name__ == "__main__":
    unittest.main()
