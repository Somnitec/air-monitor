"""Delta decoding (schema v3/v5): storage stays SPARSE — a slow metric is stored
only when the device actually measured it. The per-device cache tracks last-known
values (for the headline log / health scan), batch-seed re-transmissions are
stripped, an explicit null is a real gap, and a sensor going FS_ABSENT writes one
hard null at the transition. Pure function test — no HTTP, no DB writes."""
import json
import sqlite3
import time
import unittest

import server


def _st2(**groups):
    """Build a firmware status2 word, e.g. _st2(sen66=server.FS_ABSENT)."""
    idx = {"sen66": 0, "bh1750": 1, "bme": 2, "adxl": 3, "co": 4,
           "hcho": 5, "soil": 6, "battery": 7, "mic": 8}
    word = 0
    for name, st in groups.items():
        word |= (st & 0x3) << (2 * idx[name])
    return word


class TestDeltaDecode(unittest.TestCase):
    def setUp(self):
        server._LAST_VALUES.clear()

    def test_omitted_keys_stay_omitted_but_cache_learns_fresh_values(self):
        recs = [
            {"dev": "d", "pm25": 3.0, "co2": 800},
            {"dev": "d"},                              # not re-read -> stays sparse
            {"dev": "d", "pm25": 4.0},                 # fresh pm25 only
        ]
        server._delta_decode(recs, "d")
        self.assertNotIn("pm25", recs[1])              # no fake fresh samples
        self.assertNotIn("co2", recs[1])
        self.assertEqual(recs[2]["pm25"], 4.0)
        self.assertEqual(server._LAST_VALUES["d"]["pm25"], 4.0)
        self.assertEqual(server._LAST_VALUES["d"]["co2"], 800)

    def test_explicit_null_is_kept_and_does_not_poison_the_cache(self):
        recs = [
            {"dev": "d", "pm25": 3.0},
            {"dev": "d", "pm25": None},                # invalid read -> real gap
        ]
        server._delta_decode(recs, "d")
        self.assertIsNone(recs[1]["pm25"])             # stays null (NULL column)
        self.assertEqual(server._LAST_VALUES["d"]["pm25"], 3.0)

    def test_batch_seed_retransmission_is_stripped(self):
        """Each batch's first record carries a full slow snapshot (FS_UNCHANGED
        values) to warm a cold server cache. It must warm the cache but NOT store
        as a fresh sample — that re-created the duplicate flat-line points."""
        recs = [
            {"dev": "d", "pm25": 3.0, "co2": 800,
             "st2": _st2(sen66=server.FS_UNCHANGED)},
        ]
        server._delta_decode(recs, "d")
        self.assertNotIn("pm25", recs[0])
        self.assertNotIn("co2", recs[0])
        self.assertEqual(server._LAST_VALUES["d"]["pm25"], 3.0)   # cache warmed

    def test_fresh_read_is_stored(self):
        recs = [{"dev": "d", "pm25": 3.0, "st2": _st2(sen66=server.FS_OK)}]
        server._delta_decode(recs, "d")
        self.assertEqual(recs[0]["pm25"], 3.0)

    def test_absent_group_writes_one_null_then_goes_quiet(self):
        """The overnight-stale-SEN66 incident: sensor fails the boot probe →
        FS_ABSENT → keys omitted. The decode writes one explicit null at the
        transition (a hard gap for charts/badges) and never resurrects values."""
        recs = [
            {"dev": "d", "pm25": 3.0, "co2": 800},
            {"dev": "d", "st2": _st2(sen66=server.FS_ABSENT)},
            {"dev": "d", "st2": _st2(sen66=server.FS_ABSENT)},
        ]
        server._delta_decode(recs, "d")
        self.assertIsNone(recs[1]["pm25"])             # transition marked
        self.assertIsNone(recs[1]["co2"])
        self.assertNotIn("pm25", recs[2])              # then silence, no stale values
        self.assertNotIn("pm25", server._LAST_VALUES["d"])

    def test_absent_group_recovers_on_fresh_read(self):
        recs = [
            {"dev": "d", "pm25": 3.0},
            {"dev": "d", "st2": _st2(sen66=server.FS_ABSENT)},   # wire loose
            {"dev": "d", "pm25": 5.0},                           # rewired, fresh read
        ]
        server._delta_decode(recs, "d")
        self.assertEqual(recs[2]["pm25"], 5.0)
        self.assertEqual(server._LAST_VALUES["d"]["pm25"], 5.0)

    def test_devices_are_isolated(self):
        server._delta_decode([{"dev": "a", "pm25": 1.0}], "a")
        server._delta_decode([{"dev": "b", "pm25": 2.0}], "b")
        self.assertEqual(server._LAST_VALUES["a"]["pm25"], 1.0)
        self.assertEqual(server._LAST_VALUES["b"]["pm25"], 2.0)

    def test_default_dev_used_for_bare_records(self):
        server._delta_decode([{"pm25": 2.0}], "fallback")
        self.assertEqual(server._LAST_VALUES["fallback"]["pm25"], 2.0)


class TestColdCacheSeed(unittest.TestCase):
    """On a cold start the cache seeds from recent stored rows (newest non-null per
    key across the seed window — storage is sparse, so one row isn't enough), and
    refuses rows older than SEED_MAX_AGE_S."""

    def setUp(self):
        server._LAST_VALUES.clear()
        self._orig_conn = getattr(server, "_conn", None)
        conn = sqlite3.connect(":memory:")
        conn.row_factory = sqlite3.Row
        conn.execute(
            "CREATE TABLE readings (id INTEGER PRIMARY KEY, ts INTEGER, device TEXT, payload TEXT)"
        )
        now = int(time.time())
        # Sparse rows: co2 only in the older one, pm25 only in the newer one.
        conn.execute("INSERT INTO readings (ts, device, payload) VALUES (?,?,?)",
                     (now - 120, "d", json.dumps({"co2": 900})))
        conn.execute("INSERT INTO readings (ts, device, payload) VALUES (?,?,?)",
                     (now - 60, "d", json.dumps({"pm25": 8.0})))
        # A device whose last reading is ancient: seeding from it would resurrect
        # long-dead values after a server restart, so the seed must refuse.
        conn.execute("INSERT INTO readings (ts, device, payload) VALUES (?,?,?)",
                     (now - server.SEED_MAX_AGE_S - 60, "old",
                      json.dumps({"pm25": 9.9, "co2": 999})))
        conn.commit()
        server._conn = conn

    def tearDown(self):
        server._conn = self._orig_conn
        server._LAST_VALUES.clear()

    def test_cold_cache_seeds_newest_nonnull_per_key(self):
        cache = server._seed_fill_cache("d")
        self.assertEqual(cache["pm25"], 8.0)
        self.assertEqual(cache["co2"], 900)            # found in the older sparse row

    def test_seed_is_noop_for_unknown_device(self):
        self.assertEqual(server._seed_fill_cache("other"), {})

    def test_seed_refuses_stale_rows(self):
        self.assertEqual(server._seed_fill_cache("old"), {})


if __name__ == "__main__":
    unittest.main()
