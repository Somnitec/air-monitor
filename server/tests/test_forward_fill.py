"""Delta decoding (schema v3): the server carries forward omitted slow-channel
values, treats an explicit null as a real gap, and keeps devices isolated.
Pure function test — no HTTP, no DB writes."""
import unittest

import server


class TestForwardFill(unittest.TestCase):
    def setUp(self):
        server._LAST_VALUES.clear()

    def test_omitted_key_is_carried_forward(self):
        recs = [
            {"dev": "d", "pm25": 3.0, "co2": 800},
            {"dev": "d"},                              # unchanged -> carry both
            {"dev": "d", "pm25": 4.0},                 # pm25 fresh, co2 carried
        ]
        server._forward_fill(recs, "d")
        self.assertEqual(recs[1]["pm25"], 3.0)
        self.assertEqual(recs[1]["co2"], 800)
        self.assertEqual(recs[2]["pm25"], 4.0)
        self.assertEqual(recs[2]["co2"], 800)

    def test_explicit_null_is_a_gap_and_does_not_poison_the_cache(self):
        recs = [
            {"dev": "d", "pm25": 3.0},
            {"dev": "d", "pm25": None},                # invalid read -> real gap
            {"dev": "d"},                              # carry the last GOOD value, not null
        ]
        server._forward_fill(recs, "d")
        self.assertIsNone(recs[1]["pm25"])             # stays null (NULL column)
        self.assertEqual(recs[2]["pm25"], 3.0)         # last good carried forward

    def test_never_seen_key_stays_absent(self):
        recs = [{"dev": "d", "pm25": 3.0}]             # lux never reported
        server._forward_fill(recs, "d")
        self.assertNotIn("lux", recs[0])               # -> column NULL, not carried

    def test_devices_are_isolated(self):
        server._forward_fill([{"dev": "a", "pm25": 1.0}], "a")
        recs = [{"dev": "b"}]                          # b has no history
        server._forward_fill(recs, "b")
        self.assertNotIn("pm25", recs[0])

    def test_carries_across_separate_batches(self):
        server._forward_fill([{"dev": "d", "pm25": 9.0}], "d")
        recs = [{"dev": "d"}]                          # next /ingest, value omitted
        server._forward_fill(recs, "d")
        self.assertEqual(recs[0]["pm25"], 9.0)

    def test_default_dev_used_for_bare_records(self):
        server._forward_fill([{"pm25": 2.0}], "fallback")
        recs = [{}]
        server._forward_fill(recs, "fallback")
        self.assertEqual(recs[0]["pm25"], 2.0)


if __name__ == "__main__":
    unittest.main()
