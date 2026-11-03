#!/usr/bin/env python3
"""One maintenance poll cycle; systemd schedules the next idle cycle."""

import argparse
import json
import os
from pathlib import Path
import shutil
import ssl
import sys
import tempfile
import urllib.parse
import urllib.error
import urllib.request

if __package__:
    from .protocol import MaintenanceConfig, parse_poll
    from .device_access import load_device_config
else:
    from protocol import MaintenanceConfig, parse_poll
    from device_access import load_device_config


TERMINAL = {"succeeded", "failed", "rolled_back", "needs_attention"}


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class HttpTransport:
    def __init__(self, config: MaintenanceConfig):
        self.config = config
        context = ssl.create_default_context(cafile=config.ca_file)
        self.opener = urllib.request.build_opener(NoRedirect(), urllib.request.HTTPSHandler(context=context))

    def poll(self, payload: dict) -> dict:
        url = self.config.server_url + "/api/maintenance/v1/devices/" + urllib.parse.quote(self.config.device_id, safe="") + "/poll"
        request = urllib.request.Request(url, data=json.dumps(payload).encode(), headers=self._headers(), method="POST")
        with self.opener.open(request, timeout=30) as response:
            return json.load(response)

    def download(self, url: str, path: Path) -> None:
        request = urllib.request.Request(url, headers=self._headers())
        with self.opener.open(request, timeout=60) as response, path.open("wb") as output:
            shutil.copyfileobj(response, output)

    def _headers(self) -> dict:
        return {"Authorization": "Bearer " + self.config.device_key, "Content-Type": "application/json"}


class MaintenanceClient:
    def __init__(self, config: MaintenanceConfig, installer, state_dir: Path, transport=None):
        self.config = config
        self.installer = installer
        self.state_dir = state_dir
        self.transport = transport or HttpTransport(config)
        self.job = None

    def run_once(self) -> None:
        self.state_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
        state_path = self.state_dir / "job.json"
        if state_path.exists():
            self.job = json.loads(state_path.read_text())
            if self.job["status"] not in TERMINAL:
                self.job.update(status="needs_attention", phase="interrupted", error="Previous maintenance execution was interrupted; inspect device before another update")
                self._save()
        ack, task = self._poll()
        if self.job:
            if ack == self.job["job_id"]:
                self.job["acknowledged"] = True
                self._save()
            if not self.job.get("acknowledged"):
                return
            if task and task.job_id == self.job["job_id"]:
                return
        if task is None:
            return
        self.job = {"job_id": task.job_id, "action": task.action, "component": task.component,
                    "target_version": task.artifact.version if task.artifact else None,
                    "status": "running", "phase": "accepted", "acknowledged": False}
        self._save()
        try:
            self._poll()
        except (OSError, ValueError):
            pass
        self._execute(task)
        try:
            ack, _ = self._poll()
            if ack == task.job_id:
                self.job["acknowledged"] = True
                self._save()
        except (OSError, ValueError):
            pass

    def _execute(self, task) -> None:
        try:
            if task.action == "install":
                self.job["phase"] = "downloading"
                self._save()
                with tempfile.TemporaryDirectory(prefix="download-", dir=self.state_dir) as staging:
                    archive = Path(staging) / "release.tar.gz"
                    self.transport.download(task.artifact.url, archive)
                    release = self.installer.stage(archive, task.artifact)
                    self.job["phase"] = "installing"
                    self._save()
                    result = self.installer.apply(release, task.component, self._network_probe)
            else:
                self.job["phase"] = "installing"
                self._save()
                result = self.installer.rollback(task.component, self._network_probe)
            self.job.update(result)
            self.job["error"] = str(self.job.get("error") or "").replace(self.config.device_key, "[redacted]")[:512]
            self.job["phase"] = "completed"
        except Exception:
            mutated = self.job["phase"] == "installing"
            self.job.update(status="needs_attention" if mutated else "failed", phase="completed",
                            error="Installation requires inspection" if mutated else "Release download or validation failed")
        self._save()

    def _poll(self):
        inventory = self.installer.status()
        payload = {"schema_version": 1, "platform": "trixie-arm64", "components": inventory.get("components", inventory),
                   "services": inventory.get("services", {}), "job": self.job}
        return parse_poll(self.transport.poll(payload), self.config.server_url)

    def _network_probe(self) -> bool:
        try:
            self._poll()
            return True
        except (OSError, ValueError):
            return False

    def _save(self) -> None:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=self.state_dir, delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(self.job, stream, ensure_ascii=False)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, self.state_dir / "job.json")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    args = parser.parse_args()
    try:
        config = load_device_config(args.config)
        if config is None:
            return 0
        if __package__:
            from .install_release import Installer
        else:
            from install_release import Installer
        MaintenanceClient(config, Installer(), Path("/var/lib/cns-rpi-maintenance")).run_once()
        return 0
    except (OSError, ValueError):
        print("Maintenance poll failed; stored job result is retained", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
