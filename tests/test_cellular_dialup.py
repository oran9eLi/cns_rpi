import json
import pathlib
import subprocess
import tempfile
import unittest
from unittest import mock

from scripts.cellular_dialup import (
    InterfaceStatus,
    collect_radio_metrics,
    probe_interface,
    probe_target,
    write_snapshot_atomic,
)
from scripts.cellular_link import (
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


if __name__ == "__main__":
    unittest.main()
