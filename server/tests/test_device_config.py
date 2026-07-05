"""Two-interval device config: the server recovers the device's CURRENT fast/slow
intervals (and mode) from what the device reports, holds no override after a restart
("leave as configured"), and clears a pushed override once the device adopts it.
Pure state-function test — no HTTP, no DB."""
import unittest

import server


class TestDeviceConfig(unittest.TestCase):
    def setUp(self):
        server.DEVICE_STATE.clear()
        server.DEVICE_CONFIG.clear()
        server.PENDING_CMD.clear()

    def test_reported_intervals_land_in_device_state(self):
        server._update_device("d", "normal", 0, 1, "phase1",
                              {"poll_interval_ms": 10000, "slow_interval_ms": 180000})
        st = server.DEVICE_STATE["d"]
        self.assertEqual(st["poll_interval_ms"], 10000)
        self.assertEqual(st["slow_interval_ms"], 180000)
        self.assertEqual(st["mode"], "normal")

    def test_missing_config_keeps_last_reported_values(self):
        server._update_device("d", "normal", 0, 1, "phase1",
                              {"poll_interval_ms": 5000, "slow_interval_ms": 60000})
        server._update_device("d", "normal", 0, 1, "phase1", None)   # bare batch, no cfg
        st = server.DEVICE_STATE["d"]
        self.assertEqual(st["poll_interval_ms"], 5000)
        self.assertEqual(st["slow_interval_ms"], 60000)

    def test_no_override_by_default(self):
        server._update_device("d", "normal", 0, 1, "phase1",
                              {"poll_interval_ms": 10000})
        self.assertNotIn("d", server.DEVICE_CONFIG)   # "leave as configured"

    def test_override_clears_once_device_adopts_it(self):
        server.DEVICE_CONFIG["d"] = {"slow_interval_ms": 60000}
        # Device still on the old value -> override stands.
        server._update_device("d", "normal", 0, 1, "phase1",
                              {"slow_interval_ms": 180000})
        self.assertEqual(server.DEVICE_CONFIG["d"], {"slow_interval_ms": 60000})
        # Device now reports the target -> override drops, back to "leave as configured".
        server._update_device("d", "normal", 0, 1, "phase1",
                              {"slow_interval_ms": 60000})
        self.assertNotIn("d", server.DEVICE_CONFIG)

    def test_partial_override_only_clears_the_adopted_key(self):
        server.DEVICE_CONFIG["d"] = {"poll_interval_ms": 5000, "slow_interval_ms": 60000}
        server._update_device("d", "normal", 0, 1, "phase1",
                              {"poll_interval_ms": 5000, "slow_interval_ms": 180000})
        self.assertEqual(server.DEVICE_CONFIG["d"], {"slow_interval_ms": 60000})


if __name__ == "__main__":
    unittest.main()
