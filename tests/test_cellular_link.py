import unittest

from scripts.cellular_link import (
    CellularConfig,
    LinkState,
    LinkStateMachine,
    RecoveryBackoff,
    parse_cops,
    parse_csq,
    parse_qeng,
)


class CellularConfigTest(unittest.TestCase):
    def setUp(self):
        self.base = {
            "apn": "CMMTM5GDCDW.JS",
            "cid": 1,
            "usb_interface_number": "05",
            "at_port_wait_seconds": 30,
            "interface_name": "usb0",
        }

    def test_legacy_config_uses_recovery_defaults(self):
        config = CellularConfig.from_json(self.base)

        self.assertEqual(
            config.probe_targets,
            ("112.124.52.232", "119.29.29.29"),
        )
        self.assertEqual(config.probe_interval_seconds, 10)
        self.assertEqual(config.offline_failure_threshold, 3)
        self.assertEqual(config.online_success_threshold, 2)
        self.assertEqual(config.signal_sample_interval_seconds, 30)
        self.assertEqual(config.redial_attempts_before_reset, 3)
        self.assertEqual(config.recovery_delay_seconds, 15)
        self.assertEqual(config.recovery_delay_max_seconds, 300)

    def test_empty_probe_targets_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "probe_targets不能为空"):
            CellularConfig.from_json({**self.base, "probe_targets": []})

    def test_invalid_interval_and_backoff_are_rejected(self):
        invalid_values = (
            ("probe_interval_seconds", 0),
            ("offline_failure_threshold", 0),
            ("online_success_threshold", 0),
            ("signal_sample_interval_seconds", 0),
            ("redial_attempts_before_reset", 0),
            ("recovery_delay_seconds", 0),
            ("recovery_delay_max_seconds", 0),
        )
        for field, value in invalid_values:
            with self.subTest(field=field):
                with self.assertRaisesRegex(ValueError, field):
                    CellularConfig.from_json({**self.base, field: value})

        with self.assertRaisesRegex(ValueError, "recovery_delay_max_seconds"):
            CellularConfig.from_json(
                {
                    **self.base,
                    "recovery_delay_seconds": 30,
                    "recovery_delay_max_seconds": 15,
                }
            )


class AtResponseParserTest(unittest.TestCase):
    def test_parse_real_nr5g_sa_response(self):
        metrics = parse_qeng(
            [
                '+QENG: "servingcell","CONNECT","NR5G-SA","FDD",460,00,'
                "A08EF0011,463,100039,152650,28,30,-87,-10,-3,20,0,0"
            ]
        )

        self.assertEqual(metrics.access_technology, "NR5G-SA")
        self.assertEqual(metrics.rsrp_dbm, -87)
        self.assertEqual(metrics.rsrq_db, -10)
        self.assertEqual(metrics.sinr_db, -3)

    def test_parse_lte_response(self):
        metrics = parse_qeng(
            [
                '+QENG: "servingcell","NOCONN","LTE","FDD",460,00,'
                "1F91605,488,1300,3,5,5,4079,-101,-5,-81,11,0,0,25"
            ]
        )

        self.assertEqual(metrics.access_technology, "LTE")
        self.assertEqual(metrics.rsrp_dbm, -101)
        self.assertEqual(metrics.rsrq_db, -5)
        self.assertEqual(metrics.sinr_db, 11)

    def test_parse_nr5g_nsa_multiline_response(self):
        metrics = parse_qeng(
            [
                '+QENG: "servingcell","CONNECT"',
                '+QENG: "LTE","FDD",460,00,1F91605,488,1300,3,5,5,4079,'
                "-101,-5,-81,11,0,0,25",
                '+QENG: "NR5G-NSA",460,00,123,-88,9,-11,633984,78,ABC,100,30',
            ]
        )

        self.assertEqual(metrics.access_technology, "NR5G-NSA")
        self.assertEqual(metrics.rsrp_dbm, -88)
        self.assertEqual(metrics.rsrq_db, -11)
        self.assertEqual(metrics.sinr_db, 9)

    def test_unknown_qeng_response_returns_empty_metrics(self):
        metrics = parse_qeng(["+QENG: unknown"])

        self.assertIsNone(metrics.access_technology)
        self.assertIsNone(metrics.rsrp_dbm)
        self.assertIsNone(metrics.rsrq_db)
        self.assertIsNone(metrics.sinr_db)

    def test_parse_csq_and_unknown_value(self):
        self.assertEqual(parse_csq(["+CSQ: 19,99"]), -75)
        self.assertIsNone(parse_csq(["+CSQ: 99,99"]))
        self.assertIsNone(parse_csq(["ERROR"]))

    def test_parse_operator(self):
        self.assertEqual(
            parse_cops(['+COPS: 0,0,"China Mobile",11']),
            "China Mobile",
        )
        self.assertIsNone(parse_cops(["+COPS: 0"]))


class LinkStateMachineTest(unittest.TestCase):
    def test_three_double_failures_enter_offline(self):
        machine = LinkStateMachine(offline_threshold=3, online_threshold=2)

        self.assertEqual(
            machine.observe(True, [False, False]), LinkState.DEGRADED
        )
        self.assertEqual(
            machine.observe(True, [False, False]), LinkState.DEGRADED
        )
        self.assertEqual(machine.observe(True, [False, False]), LinkState.OFFLINE)

    def test_recovering_requires_two_full_successes(self):
        machine = LinkStateMachine(offline_threshold=3, online_threshold=2)
        machine.mark_recovering()

        self.assertEqual(machine.observe(True, [True, True]), LinkState.RECOVERING)
        self.assertEqual(machine.observe(True, [True, True]), LinkState.ONLINE)

    def test_one_reachable_target_is_degraded_without_offline(self):
        machine = LinkStateMachine(offline_threshold=3, online_threshold=2)

        for _ in range(5):
            self.assertEqual(
                machine.observe(True, [True, False]), LinkState.DEGRADED
            )

    def test_missing_basic_network_enters_offline_immediately(self):
        machine = LinkStateMachine(offline_threshold=3, online_threshold=2)

        self.assertEqual(machine.observe(False, [True, True]), LinkState.OFFLINE)

    def test_backoff_caps_and_resets(self):
        backoff = RecoveryBackoff(initial_seconds=15, maximum_seconds=300)

        self.assertEqual(
            [backoff.next_delay() for _ in range(7)],
            [15, 30, 60, 120, 240, 300, 300],
        )
        backoff.reset()
        self.assertEqual(backoff.next_delay(), 15)


if __name__ == "__main__":
    unittest.main()
