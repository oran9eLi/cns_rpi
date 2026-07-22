import dataclasses
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from datetime import datetime, timezone
from unittest import mock

from scripts.cellular_dialup import (
    CellularDaemon,
    InterfaceStatus,
    collect_radio_metrics,
    probe_interface,
    probe_latency_ms,
    probe_target,
    write_snapshot_atomic,
)
from scripts.cellular_link import (
    CellularConfig,
    CellularSnapshot,
    LinkDiagnostics,
    LinkState,
)


def sample_snapshot():
    return CellularSnapshot(
        present=True,
        link_state=LinkState.ONLINE,
        operator="China Mobile",
        access_technology="NR5G-SA",
        ip_address="100.77.73.48",
        rsrp_dbm=-87,
        rsrq_db=-10,
        sinr_db=-3,
        rssi_dbm=-75,
        tx_bytes=123,
        rx_bytes=456,
        recover_count=1,
        diagnostics=LinkDiagnostics(True, True, True, True),
    )


class RuntimeAdapterTest(unittest.TestCase):
    def test_script_entry_can_run_outside_repository_root(self):
        script = pathlib.Path(__file__).parents[1] / "scripts" / "cellular_dialup.py"

        result = subprocess.run(
            [sys.executable, str(script)],
            cwd="/tmp",
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("用法:", result.stderr)
        self.assertNotIn("ModuleNotFoundError", result.stderr)

    def test_probe_target_binds_selected_interface(self):
        runner = mock.Mock(
            return_value=subprocess.CompletedProcess([], 0, "", "")
        )

        self.assertTrue(
            probe_target("usb0", "112.124.52.232", runner=runner)
        )
        self.assertEqual(
            runner.call_args.args[0],
            [
                "ping",
                "-I",
                "usb0",
                "-c",
                "1",
                "-W",
                "2",
                "112.124.52.232",
            ],
        )

    def test_probe_target_reports_timeout_as_unreachable(self):
        runner = mock.Mock(side_effect=subprocess.TimeoutExpired("ping", 4))

        self.assertFalse(probe_target("usb0", "119.29.29.29", runner=runner))

    def test_probe_latency_parses_rtt_and_binds_selected_interface(self):
        runner = mock.Mock(
            return_value=subprocess.CompletedProcess(
                [],
                0,
                "64 bytes from 112.124.52.232: icmp_seq=1 ttl=51 time=42.7 ms\n",
                "",
            )
        )

        self.assertEqual(
            probe_latency_ms("usb0", "112.124.52.232", runner=runner),
            42.7,
        )
        self.assertEqual(
            runner.call_args.args[0],
            [
                "ping",
                "-I",
                "usb0",
                "-c",
                "1",
                "-W",
                "2",
                "112.124.52.232",
            ],
        )

    def test_probe_latency_returns_none_on_failure(self):
        failed = mock.Mock(
            return_value=subprocess.CompletedProcess([], 1, "", "")
        )
        timeout = mock.Mock(side_effect=subprocess.TimeoutExpired("ping", 4))

        self.assertIsNone(
            probe_latency_ms("usb0", "112.124.52.232", runner=failed)
        )
        self.assertIsNone(
            probe_latency_ms("usb0", "112.124.52.232", runner=timeout)
        )

    def test_write_snapshot_atomically_replaces_complete_json(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "cellular_status.json"

            write_snapshot_atomic(path, sample_snapshot())

            data = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(data["link_state"], "ONLINE")
            self.assertTrue(data["diagnostics"]["carrier_up"])
            self.assertEqual(list(path.parent.glob("*.tmp.*")), [])

    def test_probe_interface_reads_network_and_traffic_state(self):
        with tempfile.TemporaryDirectory() as directory:
            net_root = pathlib.Path(directory)
            interface = net_root / "usb0"
            (interface / "statistics").mkdir(parents=True)
            (interface / "carrier").write_text("1\n", encoding="ascii")
            (interface / "statistics" / "tx_bytes").write_text(
                "123\n", encoding="ascii"
            )
            (interface / "statistics" / "rx_bytes").write_text(
                "456\n", encoding="ascii"
            )

            def runner(command, **_kwargs):
                if command[:3] == ["ip", "-j", "address"]:
                    return subprocess.CompletedProcess(
                        command,
                        0,
                        '[{"addr_info":[{"family":"inet","local":"100.77.73.48",'
                        '"scope":"global"}]}]',
                        "",
                    )
                return subprocess.CompletedProcess(
                    command, 0, "default via 100.77.73.1 dev usb0\n", ""
                )

            status = probe_interface("usb0", net_root=net_root, runner=runner)

            self.assertEqual(
                status,
                InterfaceStatus(
                    interface_present=True,
                    carrier_up=True,
                    has_ip_address=True,
                    has_default_route=True,
                    ip_address="100.77.73.48",
                    tx_bytes=123,
                    rx_bytes=456,
                ),
            )

    def test_missing_interface_returns_empty_status(self):
        with tempfile.TemporaryDirectory() as directory:
            status = probe_interface(
                "usb0", net_root=pathlib.Path(directory), runner=mock.Mock()
            )

        self.assertEqual(status, InterfaceStatus())


class RadioCollectionTest(unittest.TestCase):
    def test_partial_at_failure_preserves_available_metrics(self):
        responses = {
            "AT+COPS?": (False, []),
            'AT+QENG="servingcell"': (
                True,
                [
                    '+QENG: "servingcell","CONNECT","NR5G-SA","FDD",460,00,'
                    "A08EF0011,463,100039,152650,28,30,-87,-10,-3,20,0,0"
                ],
            ),
            "AT+CSQ": (True, ["+CSQ: 19,99"]),
        }

        sample = collect_radio_metrics(
            object(), sender=lambda _serial, command: responses[command]
        )

        self.assertIsNone(sample.operator)
        self.assertEqual(sample.access_technology, "NR5G-SA")
        self.assertEqual(sample.rsrp_dbm, -87)
        self.assertEqual(sample.rsrq_db, -10)
        self.assertEqual(sample.sinr_db, -3)
        self.assertEqual(sample.rssi_dbm, -75)
        self.assertIn("COPS", sample.error)

    def test_all_quality_queries_can_fail_without_exception(self):
        sample = collect_radio_metrics(
            object(), sender=lambda _serial, _command: (False, [])
        )

        self.assertIsNone(sample.operator)
        self.assertIsNone(sample.access_technology)
        self.assertIsNone(sample.rssi_dbm)
        self.assertEqual(sample.error, "COPS、QENG、CSQ查询失败")


class CellularDaemonTest(unittest.TestCase):
    def setUp(self):
        self.config = CellularConfig.from_json(
            {
                "apn": "CMMTM5GDCDW.JS",
                "cid": 1,
                "usb_interface_number": "05",
                "at_port_wait_seconds": 30,
                "interface_name": "usb0",
                "probe_interval_seconds": 10,
                "signal_sample_interval_seconds": 30,
            }
        )
        self.online_interface = InterfaceStatus(
            interface_present=True,
            carrier_up=True,
            has_ip_address=True,
            has_default_route=True,
            ip_address="100.77.73.48",
            tx_bytes=123,
            rx_bytes=456,
        )

    def build_daemon(self, **overrides):
        dependencies = {
            "dialer": mock.Mock(return_value=True),
            "interface_probe": mock.Mock(return_value=self.online_interface),
            "target_probe": mock.Mock(return_value=True),
            "latency_probe": mock.Mock(return_value=42.7),
            "quality_collector": mock.Mock(
                return_value=mock.Mock(
                    operator="China Mobile",
                    access_technology="NR5G-SA",
                    rsrp_dbm=-87,
                    rsrq_db=-10,
                    sinr_db=-3,
                    rssi_dbm=-75,
                    error=None,
                )
            ),
            "module_resetter": mock.Mock(return_value=True),
            "snapshot_writer": mock.Mock(),
            "metadata_loader": mock.Mock(return_value=(0, None)),
            "wall_clock": mock.Mock(
                return_value=datetime(2026, 7, 22, 15, 30, tzinfo=timezone.utc)
            ),
            "state_logger": mock.Mock(),
        }
        dependencies.update(overrides)
        daemon = CellularDaemon(
            self.config,
            snapshot_path=pathlib.Path("/run/cns-rpi/cellular_status.json"),
            **dependencies,
        )
        return daemon, dependencies

    def test_initial_dial_is_not_repeated_while_link_is_usable(self):
        daemon, dependencies = self.build_daemon()

        self.assertEqual(daemon.run_once(0), 5)
        self.assertEqual(daemon.run_once(5), 10)
        self.assertEqual(daemon.run_once(10), 15)

        dependencies["dialer"].assert_called_once_with(self.config)
        self.assertEqual(dependencies["quality_collector"].call_count, 1)
        latest = dependencies["snapshot_writer"].call_args.args[1]
        self.assertEqual(latest.link_state, LinkState.ONLINE)

    def test_quality_is_sampled_at_independent_interval(self):
        daemon, dependencies = self.build_daemon()

        daemon.run_once(0)
        daemon.run_once(5)
        daemon.run_once(10)
        daemon.run_once(20)
        daemon.run_once(30)

        self.assertEqual(dependencies["quality_collector"].call_count, 2)

    def test_latency_is_sampled_every_five_seconds_without_accelerating_link_probe(self):
        daemon, dependencies = self.build_daemon()

        self.assertEqual(daemon.run_once(0), 5)
        self.assertEqual(daemon.run_once(5), 10)
        self.assertEqual(daemon.run_once(10), 15)

        self.assertEqual(dependencies["latency_probe"].call_count, 3)
        self.assertEqual(dependencies["target_probe"].call_count, 4)
        latest = dependencies["snapshot_writer"].call_args.args[1]
        self.assertEqual(latest.latency_ms, 42.7)
        self.assertEqual(latest.packet_loss_percent, 0.0)

    def test_latency_probe_failure_only_updates_loss_statistics(self):
        daemon, dependencies = self.build_daemon(
            latency_probe=mock.Mock(return_value=None)
        )

        daemon.run_once(0)
        daemon.run_once(5)
        daemon.run_once(10)

        latest = dependencies["snapshot_writer"].call_args.args[1]
        self.assertIsNone(latest.latency_ms)
        self.assertEqual(latest.packet_loss_percent, 100.0)
        self.assertIsNone(latest.last_error)
        self.assertEqual(latest.link_state, LinkState.ONLINE)

    def test_unavailable_at_port_marks_module_absent(self):
        daemon, dependencies = self.build_daemon(
            quality_collector=mock.Mock(return_value=None)
        )

        daemon.run_once(0)

        latest = dependencies["snapshot_writer"].call_args.args[1]
        self.assertFalse(latest.present)
        self.assertEqual(latest.last_error, "无线质量查询失败")

    def test_offline_recovery_resets_module_after_three_failed_redials(self):
        offline_config = dataclasses.replace(
            self.config,
            offline_failure_threshold=1,
            online_success_threshold=1,
            recovery_delay_seconds=15,
            recovery_delay_max_seconds=300,
        )
        dialer = mock.Mock(side_effect=[True, False, False, False])
        target_probe = mock.Mock(return_value=False)
        snapshot_writer = mock.Mock()
        daemon = CellularDaemon(
            offline_config,
            snapshot_path=pathlib.Path("/run/cns-rpi/cellular_status.json"),
            dialer=dialer,
            interface_probe=mock.Mock(return_value=self.online_interface),
            target_probe=target_probe,
            latency_probe=mock.Mock(return_value=None),
            quality_collector=mock.Mock(return_value=None),
            module_resetter=mock.Mock(return_value=True),
            snapshot_writer=snapshot_writer,
            metadata_loader=mock.Mock(return_value=(0, None)),
            wall_clock=mock.Mock(
                return_value=datetime(2026, 7, 22, 15, 30, tzinfo=timezone.utc)
            ),
            state_logger=mock.Mock(),
        )

        for now in range(0, 50, 5):
            daemon.run_once(now)

        self.assertEqual(dialer.call_count, 4)
        daemon.module_resetter.assert_called_once_with(offline_config)
        latest = snapshot_writer.call_args.args[1]
        self.assertEqual(latest.recover_count, 1)
        self.assertEqual(latest.link_state, LinkState.RECOVERING)

    def test_recovery_metadata_continues_after_service_restart(self):
        daemon, dependencies = self.build_daemon(
            metadata_loader=mock.Mock(
                return_value=(4, "2026-07-22T14:00:00.000+08:00")
            )
        )

        daemon.run_once(0)

        latest = dependencies["snapshot_writer"].call_args.args[1]
        self.assertEqual(latest.recover_count, 4)
        self.assertEqual(
            latest.last_recover_at, "2026-07-22T14:00:00.000+08:00"
        )

    def test_invalid_system_time_does_not_generate_recovery_timestamp(self):
        config = dataclasses.replace(self.config, offline_failure_threshold=1)
        daemon = CellularDaemon(
            config,
            snapshot_path=pathlib.Path("/run/cns-rpi/cellular_status.json"),
            dialer=mock.Mock(side_effect=[True, False]),
            interface_probe=mock.Mock(return_value=self.online_interface),
            target_probe=mock.Mock(return_value=False),
            latency_probe=mock.Mock(return_value=None),
            quality_collector=mock.Mock(return_value=None),
            module_resetter=mock.Mock(return_value=True),
            snapshot_writer=mock.Mock(),
            metadata_loader=mock.Mock(return_value=(0, None)),
            wall_clock=mock.Mock(
                return_value=datetime(2024, 1, 1, tzinfo=timezone.utc)
            ),
            state_logger=mock.Mock(),
        )

        daemon.run_once(0)

        self.assertIsNone(daemon.last_recover_at)

    def test_successful_redial_is_verified_before_another_redial(self):
        config = dataclasses.replace(
            self.config,
            offline_failure_threshold=1,
            online_success_threshold=1,
        )
        dialer = mock.Mock(side_effect=[True, True])
        target_probe = mock.Mock(side_effect=[False, False, True, True])
        daemon = CellularDaemon(
            config,
            snapshot_path=pathlib.Path("/run/cns-rpi/cellular_status.json"),
            dialer=dialer,
            interface_probe=mock.Mock(return_value=self.online_interface),
            target_probe=target_probe,
            latency_probe=mock.Mock(return_value=None),
            quality_collector=mock.Mock(return_value=None),
            module_resetter=mock.Mock(return_value=True),
            snapshot_writer=mock.Mock(),
            metadata_loader=mock.Mock(return_value=(0, None)),
            wall_clock=mock.Mock(
                return_value=datetime(2026, 7, 22, tzinfo=timezone.utc)
            ),
            state_logger=mock.Mock(),
        )

        self.assertEqual(daemon.run_once(0), 5)
        self.assertEqual(daemon.run_once(5), 10)
        self.assertEqual(daemon.run_once(10), 15)

        self.assertEqual(dialer.call_count, 2)
        self.assertEqual(daemon.state_machine.state, LinkState.ONLINE)

    def test_recovering_snapshot_is_written_before_blocking_redial(self):
        config = dataclasses.replace(
            self.config,
            offline_failure_threshold=1,
            online_success_threshold=1,
        )
        snapshot_writer = mock.Mock()

        dial_count = 0

        def dialer(_config):
            nonlocal dial_count
            dial_count += 1
            if dial_count == 1:
                return True
            self.assertEqual(snapshot_writer.call_count, 1)
            snapshot = snapshot_writer.call_args.args[1]
            self.assertEqual(snapshot.link_state, LinkState.RECOVERING)
            self.assertEqual(snapshot.recover_count, 1)
            return False

        daemon = CellularDaemon(
            config,
            snapshot_path=pathlib.Path("/run/cns-rpi/cellular_status.json"),
            dialer=dialer,
            interface_probe=mock.Mock(return_value=self.online_interface),
            target_probe=mock.Mock(return_value=False),
            latency_probe=mock.Mock(return_value=None),
            quality_collector=mock.Mock(return_value=None),
            module_resetter=mock.Mock(return_value=True),
            snapshot_writer=snapshot_writer,
            metadata_loader=mock.Mock(return_value=(0, None)),
            wall_clock=mock.Mock(
                return_value=datetime(2026, 7, 22, tzinfo=timezone.utc)
            ),
            state_logger=mock.Mock(),
        )

        daemon.run_once(0)


if __name__ == "__main__":
    unittest.main()
