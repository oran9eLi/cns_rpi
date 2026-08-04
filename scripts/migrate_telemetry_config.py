#!/usr/bin/env python3
"""将旧版遥测配置幂等迁移为快照/实时双通道结构。"""

import json
import os
from pathlib import Path
import stat
import sys
import tempfile


SNAPSHOT_SUFFIX = "telemetry/snapshot/v1"
REALTIME_SUFFIX = "telemetry/realtime/v1"


def _integer(value: object, minimum: int, maximum: int) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and minimum <= value <= maximum


def _channel_valid(channel: object, minimum: int, maximum: int) -> bool:
    return (
        isinstance(channel, dict)
        and isinstance(channel.get("enabled"), bool)
        and _integer(channel.get("interval_ms"), minimum, maximum)
    )


def _topic_valid(topic: object, suffix: str) -> bool:
    return (
        isinstance(topic, dict)
        and topic.get("suffix") == suffix
        and topic.get("qos") == 0
    )


def _is_new_configuration(root: object) -> bool:
    if not isinstance(root, dict):
        return False
    publish = root.get("telemetry_publish")
    mqtt = root.get("mqtt")
    if not isinstance(publish, dict) or not isinstance(mqtt, dict):
        return False
    topics = mqtt.get("topics")
    return (
        isinstance(topics, dict)
        and _channel_valid(publish.get("snapshot"), 100, 60000)
        and _channel_valid(publish.get("realtime"), 50, 1000)
        and _topic_valid(topics.get("telemetry_snapshot"), SNAPSHOT_SUFFIX)
        and _topic_valid(topics.get("telemetry_realtime"), REALTIME_SUFFIX)
        and "telemetry_publish_interval_ms" not in root.get("runtime", {})
        and "px4_realtime" not in root
        and "telemetry" not in topics
    )


def _migrate(root: object) -> dict:
    if not isinstance(root, dict):
        raise ValueError("配置根节点必须是对象")
    runtime = root.get("runtime")
    mqtt = root.get("mqtt")
    topics = mqtt.get("topics") if isinstance(mqtt, dict) else None
    old_realtime = root.get("px4_realtime", {})
    if not isinstance(runtime, dict) or not isinstance(topics, dict):
        raise ValueError("缺少runtime或mqtt.topics配置")
    if "telemetry_publish_interval_ms" not in runtime or "telemetry" not in topics:
        raise ValueError("旧版遥测配置不完整")
    if not isinstance(old_realtime, dict):
        raise ValueError("px4_realtime必须是对象")

    snapshot_ms = runtime["telemetry_publish_interval_ms"]
    realtime_ms = old_realtime.get("publish_interval_ms", 100)
    realtime_enabled = old_realtime.get("enabled", True)
    if not _integer(snapshot_ms, 100, 60000):
        raise ValueError("旧快照周期超出允许范围")
    if realtime_ms == 50:
        realtime_ms = 100
    if not _integer(realtime_ms, 50, 1000) or not isinstance(realtime_enabled, bool):
        raise ValueError("旧实时通道配置无法迁移")

    migrated = dict(root)
    migrated_runtime = dict(runtime)
    migrated_runtime.pop("telemetry_publish_interval_ms")
    migrated["runtime"] = migrated_runtime
    migrated.pop("px4_realtime", None)
    migrated["telemetry_publish"] = {
        "snapshot": {"enabled": True, "interval_ms": snapshot_ms},
        "realtime": {"enabled": realtime_enabled, "interval_ms": realtime_ms},
    }

    migrated_mqtt = dict(mqtt)
    migrated_topics = dict(topics)
    migrated_topics.pop("telemetry")
    migrated_topics["telemetry_snapshot"] = {"suffix": SNAPSHOT_SUFFIX, "qos": 0}
    migrated_topics["telemetry_realtime"] = {"suffix": REALTIME_SUFFIX, "qos": 0}
    migrated_mqtt["topics"] = migrated_topics
    migrated["mqtt"] = migrated_mqtt
    return migrated


def migrate_file(path: Path) -> None:
    original = path.read_bytes()
    root = json.loads(original.decode("utf-8"))
    if _is_new_configuration(root):
        return

    migrated = _migrate(root)
    mode = stat.S_IMODE(path.stat().st_mode)
    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=path.parent,
            prefix=f".{path.name}.",
            suffix=".tmp",
            delete=False,
        ) as output:
            temporary_path = Path(output.name)
            json.dump(migrated, output, ensure_ascii=False, indent=2)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        os.chmod(temporary_path, mode)
        os.replace(temporary_path, path)
        temporary_path = None
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"用法：{argv[0]} <config.json>", file=sys.stderr)
        return 1
    try:
        migrate_file(Path(argv[1]))
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError, TypeError) as error:
        print(f"遥测发布配置迁移失败：{error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
