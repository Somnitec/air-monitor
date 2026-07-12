"""Runway-use ingester: parse the LVNL feed, classify overhead, upsert without dups."""
import sqlite3
import unittest

import runway


SAMPLE = {
    "title": "Schiphol Runway Usage",
    "times": [
        {"from": "2026-07-12T00:00:00+00:00", "until": "2026-07-12T04:45:00+00:00",
         "landingRunways": ["06"], "departingRunways": ["36L"]},
        # Buitenveldertbaan westerly config: landing 27 = approach over the station.
        {"from": "2026-07-12T12:00:00+00:00", "until": "2026-07-12T14:00:00+00:00",
         "landingRunways": ["27", "18R"], "departingRunways": ["24"]},
        {"from": "bad-timestamp", "until": "x", "landingRunways": ["09"]},  # dropped
    ],
}


class TestParse(unittest.TestCase):
    def test_parse_and_overhead_flag(self):
        iv = runway.parse_intervals(SAMPLE)
        self.assertEqual(len(iv), 2)                      # malformed row skipped
        self.assertFalse(iv[0]["overhead"])              # landing 06 / dep 36L → not overhead
        self.assertTrue(iv[1]["overhead"])               # landing 27 → overhead
        self.assertEqual(iv[0]["landing"], ["06"])

    def test_is_overhead(self):
        self.assertTrue(runway.is_overhead(["27"], []))        # land 27
        self.assertTrue(runway.is_overhead([], ["09"]))        # depart 09
        self.assertFalse(runway.is_overhead(["09"], ["27"]))   # opposite direction, over Schiphol not us
        self.assertFalse(runway.is_overhead(["06", "36R"], ["36L"]))


class TestStore(unittest.TestCase):
    def setUp(self):
        self.conn = sqlite3.connect(":memory:")
        self.conn.row_factory = sqlite3.Row
        runway.init_table(self.conn)

    def test_upsert_is_idempotent_and_grows_live_interval(self):
        iv = runway.parse_intervals(SAMPLE)
        runway.upsert(self.conn, iv, now=1000)
        runway.upsert(self.conn, iv, now=1001)            # same intervals re-fetched
        n = self.conn.execute("SELECT COUNT(*) FROM runway_use").fetchone()[0]
        self.assertEqual(n, 2)                            # keyed on from_ts → no dups

        # Live interval's `until` extends on the next poll: same from_ts, later until.
        iv[1]["until_ts"] += 300
        runway.upsert(self.conn, iv, now=1002)
        self.assertEqual(self.conn.execute("SELECT COUNT(*) FROM runway_use").fetchone()[0], 2)
        cur = runway.latest(self.conn)
        self.assertTrue(cur["overhead"])
        self.assertEqual(cur["landing"], ["27", "18R"])

    def test_query_since(self):
        runway.upsert(self.conn, runway.parse_intervals(SAMPLE), now=1000)
        # since after the first interval's until → only the later one returned
        got = runway.query(self.conn, since=runway._epoch("2026-07-12T05:00:00+00:00"))
        self.assertEqual(len(got), 1)
        self.assertTrue(got[0]["overhead"])


class TestMaintenance(unittest.TestCase):
    CLOSURES = [
        {"runway": "09/27", "name": "Buitenveldertbaan", "from": "2026-08-31", "until": "2026-09-08"},
        {"runway": "18R/36L", "name": "Polderbaan", "from": "2026-10-01", "until": "2026-10-05"},
    ]

    def test_affects_overhead(self):
        self.assertTrue(runway._affects_overhead(self.CLOSURES[0]))    # Buitenveldertbaan
        self.assertFalse(runway._affects_overhead(self.CLOSURES[1]))   # Polderbaan (N-S)

    def test_current_vs_upcoming(self):
        during = runway._day_bounds("2026-08-31", "2026-09-08")[0] + 3600  # within the closure
        st = runway.maintenance_status(self.CLOSURES, now=during)
        self.assertEqual(len(st["current"]), 1)
        self.assertTrue(st["current"][0]["affects_overhead"])
        self.assertEqual(st["current"][0]["name"], "Buitenveldertbaan")
        self.assertEqual([c["name"] for c in st["upcoming"]], ["Polderbaan"])

    def test_past_closures_dropped(self):
        after = runway._day_bounds("2026-10-01", "2026-10-05")[1] + 86400
        st = runway.maintenance_status(self.CLOSURES, now=after)
        self.assertEqual(st["current"], [])
        self.assertEqual(st["upcoming"], [])

    def test_inclusive_until_day(self):
        # 'until' covers the whole final day: late evening on Sep 8 is still closed.
        _, end = runway._day_bounds("2026-08-31", "2026-09-08")
        # end must be at/after 23:00 local on Sep 8
        self.assertGreater(end, runway._day_bounds("2026-09-08", "2026-09-08")[0] + 22*3600)


if __name__ == "__main__":
    unittest.main()
