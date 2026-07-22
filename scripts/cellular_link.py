#!/usr/bin/env python3
"""5G 链路守护服务的配置、AT 响应解析和状态机。"""

from __future__ import annotations

import csv
import collections
import dataclasses
import enum
import re
from typing import Optional


DEFAULT_PROBE_TARGETS = ("112.124.52.232",)


class LinkState(enum.Enum):
    """5G 数据链路的对外状态。"""

    UNKNOWN = "UNKNOWN"
    ONLINE = "ONLINE"
    DEGRADED = "DEGRADED"
    OFFLINE = "OFFLINE"
    RECOVERING = "RECOVERING"


@dataclasses.dataclass(frozen=True)
class RadioMetrics:
    """服务小区响应中可稳定取得的无线质量指标。"""

    access_technology: Optional[str] = None
    rsrp_dbm: Optional[int] = None
    rsrq_db: Optional[int] = None
    sinr_db: Optional[int] = None


@dataclasses.dataclass(frozen=True)
class CellularConfig:
    """5G 守护服务配置。新增字段缺失时使用兼容默认值。"""

    apn: str
    cid: int
    usb_interface_number: str
    at_port_wait_seconds: int
    interface_name: str = "usb0"
    probe_targets: tuple[str, ...] = DEFAULT_PROBE_TARGETS
    probe_interval_seconds: int = 10
    quality_probe_interval_seconds: int = 5
    offline_failure_threshold: int = 3
    degraded_failure_threshold: int = 2
    online_success_threshold: int = 2
    signal_sample_interval_seconds: int = 30
    redial_attempts_before_reset: int = 3
    recovery_delay_seconds: int = 15
    recovery_delay_max_seconds: int = 300

    @classmethod
    def from_json(cls, value: dict) -> "CellularConfig":
        """解析 config.json 的 cellular 节并验证恢复参数。"""

        if not isinstance(value, dict):
            raise ValueError("cellular必须是对象")
        try:
            config = cls(
                apn=_required_string(value, "apn"),
                cid=_required_positive_int(value, "cid"),
                usb_interface_number=_required_string(
                    value, "usb_interface_number"
                ),
                at_port_wait_seconds=_required_positive_int(
                    value, "at_port_wait_seconds"
                ),
                interface_name=_optional_string(value, "interface_name", "usb0"),
                probe_targets=_parse_probe_targets(value),
                probe_interval_seconds=_optional_positive_int(
                    value, "probe_interval_seconds", 10
                ),
                quality_probe_interval_seconds=_optional_positive_int(
                    value, "quality_probe_interval_seconds", 5
                ),
                offline_failure_threshold=_optional_positive_int(
                    value, "offline_failure_threshold", 3
                ),
                degraded_failure_threshold=_optional_positive_int(
                    value, "degraded_failure_threshold", 2
                ),
                online_success_threshold=_optional_positive_int(
                    value, "online_success_threshold", 2
                ),
                signal_sample_interval_seconds=_optional_positive_int(
                    value, "signal_sample_interval_seconds", 30
                ),
                redial_attempts_before_reset=_optional_positive_int(
                    value, "redial_attempts_before_reset", 3
                ),
                recovery_delay_seconds=_optional_positive_int(
                    value, "recovery_delay_seconds", 15
                ),
                recovery_delay_max_seconds=_optional_positive_int(
                    value, "recovery_delay_max_seconds", 300
                ),
            )
        except KeyError as exc:
            raise ValueError(f"缺少cellular.{exc.args[0]}") from exc

        if config.recovery_delay_max_seconds < config.recovery_delay_seconds:
            raise ValueError(
                "recovery_delay_max_seconds不能小于recovery_delay_seconds"
            )
        if config.degraded_failure_threshold >= config.offline_failure_threshold:
            raise ValueError(
                "degraded_failure_threshold必须小于offline_failure_threshold"
            )
        return config


@dataclasses.dataclass(frozen=True)
class LinkDiagnostics:
    """仅供树莓派生成 RPICELL 位图的内部链路诊断值。"""

    interface_present: bool = False
    carrier_up: bool = False
    has_ip_address: bool = False
    has_default_route: bool = False


@dataclasses.dataclass(frozen=True)
class CellularSnapshot:
    """写入 /run 的跨进程 5G 状态快照。"""

    present: bool = False
    link_state: LinkState = LinkState.UNKNOWN
    operator: Optional[str] = None
    access_technology: Optional[str] = None
    ip_address: Optional[str] = None
    rsrp_dbm: Optional[int] = None
    rsrq_db: Optional[int] = None
    sinr_db: Optional[int] = None
    rssi_dbm: Optional[int] = None
    tx_bytes: Optional[int] = None
    rx_bytes: Optional[int] = None
    latency_ms: Optional[float] = None
    packet_loss_percent: Optional[float] = None
    recover_count: int = 0
    last_recover_at: Optional[str] = None
    last_error: Optional[str] = None
    diagnostics: LinkDiagnostics = dataclasses.field(default_factory=LinkDiagnostics)

    def to_dict(self, include_diagnostics: bool = True) -> dict:
        result = {
            "present": self.present,
            "link_state": self.link_state.value,
            "operator": self.operator,
            "access_technology": self.access_technology,
            "ip_address": self.ip_address,
            "rsrp_dbm": self.rsrp_dbm,
            "rsrq_db": self.rsrq_db,
            "sinr_db": self.sinr_db,
            "rssi_dbm": self.rssi_dbm,
            "tx_bytes": self.tx_bytes,
            "rx_bytes": self.rx_bytes,
            "latency_ms": self.latency_ms,
            "packet_loss_percent": self.packet_loss_percent,
            "recover_count": self.recover_count,
            "last_recover_at": self.last_recover_at,
            "last_error": self.last_error,
        }
        if include_diagnostics:
            result["diagnostics"] = dataclasses.asdict(self.diagnostics)
        return result


class LinkQualityWindow:
    """保存固定数量的 RTT 样本并计算平均延迟与丢包率。"""

    def __init__(self, capacity: int = 12):
        if capacity <= 0:
            raise ValueError("质量统计窗口容量必须为正整数")
        self._samples = collections.deque(maxlen=capacity)

    def add(self, latency_ms: Optional[float]) -> None:
        self._samples.append(latency_ms)

    def snapshot(self) -> tuple[Optional[float], Optional[float]]:
        if not self._samples:
            return None, None
        successful = [value for value in self._samples if value is not None]
        latency_ms = (
            round(sum(successful) / len(successful), 1) if successful else None
        )
        lost = len(self._samples) - len(successful)
        packet_loss_percent = round(lost * 100.0 / len(self._samples), 1)
        return latency_ms, packet_loss_percent


class LinkStateMachine:
    """对业务入口探测结果应用降级、离线和上线迟滞。"""

    def __init__(
        self,
        offline_threshold: int,
        online_threshold: int,
        degraded_threshold: int = 2,
    ):
        if (
            offline_threshold <= 0
            or online_threshold <= 0
            or degraded_threshold <= 0
            or degraded_threshold >= offline_threshold
        ):
            raise ValueError("状态迟滞阈值必须为正整数")
        self._offline_threshold = offline_threshold
        self._online_threshold = online_threshold
        self._degraded_threshold = degraded_threshold
        self._failure_count = 0
        self._success_count = 0
        self.state = LinkState.UNKNOWN

    def mark_recovering(self) -> None:
        self.state = LinkState.RECOVERING
        self._failure_count = 0
        self._success_count = 0

    def observe(self, basic_ready: bool, target_results: list[bool]) -> LinkState:
        if not basic_ready:
            self._failure_count = self._offline_threshold
            self._success_count = 0
            self.state = LinkState.OFFLINE
            return self.state

        reachable = bool(target_results) and target_results[0]
        if reachable:
            self._failure_count = 0
            self._success_count += 1
            if self.state == LinkState.ONLINE:
                return self.state
            if self._success_count >= self._online_threshold:
                self.state = LinkState.ONLINE
            elif self.state not in (LinkState.RECOVERING, LinkState.UNKNOWN):
                self.state = LinkState.DEGRADED
            return self.state

        self._success_count = 0
        self._failure_count += 1
        if self._failure_count >= self._offline_threshold:
            self.state = LinkState.OFFLINE
        elif (
            self._failure_count >= self._degraded_threshold
            or self.state != LinkState.ONLINE
        ):
            self.state = LinkState.DEGRADED
        else:
            self.state = LinkState.ONLINE
        return self.state


class RecoveryBackoff:
    """上限受控的二倍指数退避。"""

    def __init__(self, initial_seconds: int, maximum_seconds: int):
        if initial_seconds <= 0 or maximum_seconds < initial_seconds:
            raise ValueError("恢复退避参数无效")
        self._initial = initial_seconds
        self._maximum = maximum_seconds
        self._next = initial_seconds

    def next_delay(self) -> int:
        current = self._next
        self._next = min(self._next * 2, self._maximum)
        return current

    def reset(self) -> None:
        self._next = self._initial


def parse_qeng(lines: list[str]) -> RadioMetrics:
    """解析移远 QENG 服务小区响应中的接入制式和无线质量。"""

    rows = [_parse_at_csv(line) for line in lines if line.startswith("+QENG:")]
    rows = [row for row in rows if row]
    for row in rows:
        if row[0] == "NR5G-NSA" and len(row) > 6:
            return RadioMetrics(
                access_technology="NR5G-NSA",
                rsrp_dbm=_to_int(row[4]),
                rsrq_db=_to_int(row[6]),
                sinr_db=_to_int(row[5]),
            )
    for row in rows:
        if len(row) > 14 and row[0] == "servingcell" and row[2] == "NR5G-SA":
            return RadioMetrics(
                access_technology="NR5G-SA",
                rsrp_dbm=_to_int(row[12]),
                rsrq_db=_to_int(row[13]),
                sinr_db=_to_int(row[14]),
            )
        if len(row) > 16 and row[0] == "servingcell" and row[2] == "LTE":
            return RadioMetrics(
                access_technology="LTE",
                rsrp_dbm=_to_int(row[13]),
                rsrq_db=_to_int(row[14]),
                sinr_db=_to_int(row[16]),
            )
    return RadioMetrics()


def parse_csq(lines: list[str]) -> Optional[int]:
    """把 3GPP CSQ 的 RSSI 索引换算为 dBm。"""

    for line in lines:
        match = re.fullmatch(r"\+CSQ:\s*(\d+),(\d+)", line.strip())
        if match is None:
            continue
        rssi = int(match.group(1))
        return None if rssi == 99 else -113 + 2 * rssi
    return None


def parse_cops(lines: list[str]) -> Optional[str]:
    """解析 COPS 响应中的运营商名称。"""

    for line in lines:
        if not line.startswith("+COPS:"):
            continue
        row = _parse_at_csv(line)
        if len(row) >= 3 and row[2]:
            return row[2]
    return None


def _parse_at_csv(line: str) -> list[str]:
    if ":" not in line:
        return []
    try:
        return next(csv.reader([line.split(":", 1)[1].strip()], skipinitialspace=True))
    except (csv.Error, StopIteration):
        return []


def _to_int(value: str) -> Optional[int]:
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _required_string(value: dict, field: str) -> str:
    result = value[field]
    if not isinstance(result, str) or not result.strip():
        raise ValueError(f"{field}必须是非空字符串")
    return result


def _optional_string(value: dict, field: str, default: str) -> str:
    if field not in value:
        return default
    return _required_string(value, field)


def _required_positive_int(value: dict, field: str) -> int:
    result = value[field]
    if isinstance(result, bool) or not isinstance(result, int) or result <= 0:
        raise ValueError(f"{field}必须是正整数")
    return result


def _optional_positive_int(value: dict, field: str, default: int) -> int:
    if field not in value:
        return default
    return _required_positive_int(value, field)


def _parse_probe_targets(value: dict) -> tuple[str, ...]:
    targets = value.get("probe_targets", list(DEFAULT_PROBE_TARGETS))
    if not isinstance(targets, list) or not targets:
        raise ValueError("probe_targets不能为空")
    if any(not isinstance(target, str) or not target.strip() for target in targets):
        raise ValueError("probe_targets必须包含非空字符串")
    return tuple(targets)
