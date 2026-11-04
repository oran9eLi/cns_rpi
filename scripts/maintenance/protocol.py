"""Maintenance HTTP boundary types, independent of the business MQTT protocol."""

from dataclasses import dataclass
import ipaddress
import re
from urllib.parse import urlsplit


@dataclass(frozen=True)
class Artifact:
    version: str
    git_commit: str
    platform: str
    url: str
    sha256: str


@dataclass(frozen=True)
class Job:
    job_id: str
    action: str
    component: str
    artifact: Artifact | None = None


@dataclass(frozen=True)
class MaintenanceConfig:
    server_url: str
    device_id: str
    device_key: str
    ca_file: str | None = None


def parse_config(raw: dict) -> MaintenanceConfig:
    url, ca_file = parse_server_config(raw)
    device_id = parse_device_id(raw.get('device_id'))
    device_key = _text(raw, 'device_key')
    if '\x7f' in device_key:
        raise ValueError('invalid device_key')
    return MaintenanceConfig(url, device_id, device_key, ca_file)


def parse_server_config(raw: dict) -> tuple[str, str | None]:
    _schema(raw)
    url = _text(raw, 'server_url').rstrip('/')
    _origin(url)
    parsed = urlsplit(url)
    if parsed.query or parsed.fragment:
        raise ValueError('server_url must not contain query or fragment')
    ca_file = raw.get('ca_file')
    if ca_file is not None and (not isinstance(ca_file, str) or not ca_file):
        raise ValueError('invalid ca_file')
    return url, ca_file


def parse_device_id(value) -> str:
    if not isinstance(value, str) or not re.fullmatch(r'[A-Za-z0-9_.:-]{1,20}', value) or value in ('.', '..'):
        raise ValueError('invalid device_id')
    return value


def parse_artifact(raw: dict, allowed_origin: str) -> Artifact:
    if not isinstance(raw, dict):
        raise ValueError("artifact must be an object")
    version = _text(raw, "version")
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?(?:\+[A-Za-z0-9.-]+)?", version):
        raise ValueError("invalid version")
    commit = _text(raw, "git_commit").lower()
    digest = _text(raw, "sha256").lower()
    if not re.fullmatch(r"[0-9a-f]{40}", commit) or not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise ValueError("invalid release digest or commit")
    if raw.get("platform") != "trixie-arm64":
        raise ValueError("unsupported release platform")
    url = _text(raw, "artifact_url")
    if _origin(url) != _origin(allowed_origin):
        raise ValueError("artifact must use the configured server origin")
    if urlsplit(url).fragment:
        raise ValueError("artifact URL must not contain fragment")
    return Artifact(version, commit, "trixie-arm64", url, digest)


def parse_poll(raw: dict, allowed_origin: str) -> tuple[str | None, Job | None]:
    _schema(raw)
    access = raw.get('access')
    if access not in ('pending', 'approved', 'rejected'):
        raise ValueError('invalid device access status')
    if access != 'approved':
        if raw.get('acknowledged_job_id') is not None or raw.get('job') is not None:
            raise ValueError('unapproved device cannot receive a task or acknowledgement')
        return None, None
    ack = raw.get("acknowledged_job_id")
    if ack is not None:
        ack = _identifier(raw, "acknowledged_job_id")
    item = raw.get("job")
    if item is None:
        return ack, None
    if not isinstance(item, dict):
        raise ValueError("job must be an object")
    job_id = _identifier(item, "job_id")
    action = item.get("action")
    component = item.get("component")
    if action not in ("install", "rollback") or component not in ("app", "cellular"):
        raise ValueError("unsupported maintenance action or component")
    artifact = parse_artifact(item, allowed_origin) if action == "install" else None
    return ack, Job(job_id, action, component, artifact)


def _schema(raw: dict) -> None:
    if not isinstance(raw, dict) or type(raw.get("schema_version")) is not int or raw["schema_version"] != 1:
        raise ValueError("unsupported maintenance schema")


def _text(raw: dict, name: str) -> str:
    value = raw.get(name)
    if not isinstance(value, str) or not value or any(ord(char) < 32 for char in value):
        raise ValueError("invalid " + name)
    return value


def _identifier(raw: dict, name: str) -> str:
    value = _text(raw, name)
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}", value):
        raise ValueError("invalid " + name)
    return value


def _origin(url: str) -> tuple[str, str, int]:
    parts = urlsplit(url)
    if parts.username is not None or parts.password is not None or not parts.hostname:
        raise ValueError("invalid maintenance URL")
    try:
        loopback = ipaddress.ip_address(parts.hostname).is_loopback
    except ValueError:
        loopback = parts.hostname == "localhost"
    if parts.scheme != "https" and not (parts.scheme == "http" and loopback):
        raise ValueError("HTTPS is required except on loopback")
    return parts.scheme, parts.hostname.lower(), parts.port or (443 if parts.scheme == "https" else 80)
