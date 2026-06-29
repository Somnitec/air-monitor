"""One aircraft poll tick end-to-end: read fixture -> snapshot callback + throttled log."""
import asyncio
import sqlite3
import unittest
from pathlib import Path
from unittest.mock import patch

from aircraft import scheduler, store
from aircraft.scheduler import poll_once

FIXTURE = Path(__file__).parent / "fixtures" / "aircraft_sample.json"


def _settings(**kw):
    s = dict(enabled=True, lat=52.179722, lon=5.284722,
             json_path=str(FIXTURE), json_url=None,
             poll_sec=1.0, stale_sec=60.0, max_range_km=300.0, log_sec=15.0)
    s.update(kw)
    return s


class TestPollOnce(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.conn = sqlite3.connect(":memory:")
        self.conn.row_factory = sqlite3.Row
        store.init_aircraft_table(self.conn)
        self.lock = asyncio.Lock()

    def _count(self):
        return self.conn.execute("SELECT COUNT(*) FROM aircraft").fetchone()[0]

    async def test_tick_broadcasts_and_logs_in_range(self):
        seen = []

        async def on_snapshot(recs):
            seen.append(recs)

        recs = await poll_once(self.conn, self.lock, settings=_settings(),
                               last_logged={}, on_snapshot=on_snapshot)
        self.assertEqual(len(recs), 4)        # 4 in-range aircraft in the fixture
        self.assertEqual(len(seen), 1)        # snapshot pushed once
        self.assertEqual(len(seen[0]), 4)
        self.assertEqual(self._count(), 4)    # all logged on first sighting

    async def test_throttle_skips_second_tick_log(self):
        last = {}
        await poll_once(self.conn, self.lock, settings=_settings(), last_logged=last)
        await poll_once(self.conn, self.lock, settings=_settings(), last_logged=last)
        self.assertEqual(self._count(), 4)    # within log_sec -> no new rows

    async def test_missing_feed_is_empty_not_error(self):
        recs = await poll_once(self.conn, self.lock,
                               settings=_settings(json_path="/no/such.json"),
                               last_logged={})
        self.assertEqual(recs, [])
        self.assertEqual(self._count(), 0)

    async def test_sdr_poll_runs_when_public_feed_is_down(self):
        """No internet: the public refresh raises, but the local SDR poll must still
        run (the readsb feed needs no internet) and the stale public copy is dropped."""
        seen = {}
        settings = _settings(public_enabled=True, public_poll_sec=0.0, poll_sec=0.0)

        async def boom(_s):                       # internet unreachable
            raise RuntimeError("no internet")

        async def fake_poll(conn, lock, *, settings, last_logged,
                            on_snapshot=None, public_records=None):
            seen["reached"] = True
            seen["public_records"] = public_records
            raise asyncio.CancelledError          # break out of the infinite loop

        with patch.object(scheduler, "_refresh_public", boom), \
             patch.object(scheduler, "poll_once", fake_poll):
            with self.assertRaises(asyncio.CancelledError):
                await scheduler.run_loop(self.conn, self.lock, settings=settings)

        self.assertTrue(seen.get("reached"))      # SDR poll ran despite the public failure
        self.assertEqual(seen["public_records"], [])  # stale internet copy dropped


if __name__ == "__main__":
    unittest.main()
