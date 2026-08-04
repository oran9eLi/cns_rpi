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


def _topic_path_valid(value: object) -> bool:
    return (
        isinstance(value, str)
        and bool(value)
        and not value.startswith("/")
        and not value.endswith("/")
        and "+" not in value
        and "#" not in value
        and "//" not in value
    )


def _topic_valid(topic: object) -> bool:
    return (
        isinstance(topic, dict)
        and _topic_path_valid(topic.get("suffix"))
        and _integer(topic.get("qos"), 0, 0)
    )


def _applied_command_ids_valid(value: object) -> bool:
    if not isinstance(value, list) or len(value) > 32:
        return False
    return all(
        isinstance(command_id, str) and 0 < len(command_id) <= 128
        for command_id in value
    ) and len(set(value)) == len(value)


def _is_new_configuration(root: object) -> bool:
    if not isinstance(root, dict):
        return False
    publish = root.get("telemetry_publish")
    mqtt = root.get("mqtt")
    runtime = root.get("runtime")
    if (
        not isinstance(publish, dict)
        or not isinstance(mqtt, dict)
        or not isinstance(runtime, dict)
    ):
        return False
    topics = mqtt.get("topics")
    snapshot_topic = topics.get("telemetry_snapshot") if isinstance(topics, dict) else None
    realtime_topic = topics.get("telemetry_realtime") if isinstance(topics, dict) else None
    return (
        isinstance(topics, dict)
        and _channel_valid(publish.get("snapshot"), 100, 60000)
        and _channel_valid(publish.get("realtime"), 50, 1000)
        and _topic_valid(snapshot_topic)
        and _topic_valid(realtime_topic)
        and snapshot_topic["suffix"] != realtime_topic["suffix"]
        and _integer(runtime.get("heartbeat_interval_ms"), 100, 60000)
        and _applied_command_ids_valid(runtime.get("applied_command_ids"))
        and "telemetry_publish_interval_ms" not in runtime
        and "px4_realtime" not in root
        and "telemetry" not in topics
    )


def _configuration_shape(root: object) -> str:
    if not isinstance(root, dict):
        raise ValueError("配置根节点必须是对象")
    runtime = root.get("runtime")
    mqtt = root.get("mqtt")
    topics = mqtt.get("topics") if isinstance(mqtt, dict) else None
    has_new = "telemetry_publish" in root or (
        isinstance(topics, dict)
        and ("telemetry_snapshot" in topics or "telemetry_realtime" in topics)
    )
    has_old = "px4_realtime" in root or (
        isinstance(runtime, dict) and "telemetry_publish_interval_ms" in runtime
    ) or (isinstance(topics, dict) and "telemetry" in topics)
    if has_new and has_old:
        raise ValueError("新旧遥测配置不能混用")
    if has_new:
        if not _is_new_configuration(root):
            raise ValueError("新版遥测配置结构非法")
        return "new"
    if has_old:
        return "old"
    raise ValueError("缺少遥测发布配置")


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
    if _configuration_shape(root) == "new":
        return

    migrated = _migrate(root)
    if not _is_new_configuration(migrated):
        raise ValueError("迁移结果不符合新版遥测配置结构")
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
