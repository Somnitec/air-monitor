"""Tests for the weather SQLite store: schema, idempotent upsert, queries."""
import sqlite3
import unittest

from weather.base import WeatherObs
from weather import store


def _obs(ts=1700000000, source="open_meteo", station="", kind="obs", **values):
    return WeatherObs(source, station, kind, ts, 52.18, 5.28, 1.2, values or {"temp": 14.0})


class TestStore(unittest.TestCase):
    def setUp(self):
        self.conn = sqlite3.connect(":memory:")
        self.conn.row_factory = sqlite3.Row
        store.init_weather_table(self.conn)

    def _count(self):
        return self.conn.execute("SELECT COUNT(*) FROM weather").fetchone()[0]

    def test_insert_one(self):
        n = store.upsert(self.conn, [_obs(temp=12.5)])
        self.assertEqual(n, 1)
        self.assertEqual(self._count(), 1)

    def test_reinsert_same_is_idempotent(self):
        store.upsert(self.conn, [_obs(temp=12.5)])
        store.upsert(self.conn, [_obs(temp=12.5)])
        self.assertEqual(self._count(), 1)

    def test_reinsert_updates_changed_value(self):
        store.upsert(self.conn, [_obs(temp=12.5)])
        store.upsert(self.conn, [_obs(temp=13.9)])           # same dedupe key, new value
        self.assertEqual(self._count(), 1)
        row = self.conn.execute("SELECT payload FROM weather").fetchone()
        self.assertIn("13.9", row["payload"])

    def test_different_valid_ts_makes_a_new_row(self):
        store.upsert(self.conn, [_obs(ts=1700000000), _obs(ts=1700003600)])
        self.assertEqual(self._count(), 2)

    def test_different_source_same_time_coexist(self):
        store.upsert(self.conn, [_obs(source="open_meteo"), _obs(source="knmi", station="06260")])
        self.assertEqual(self._count(), 2)

    def test_query_filters_by_range_and_source(self):
        store.upsert(self.conn, [
            _obs(ts=1700000000, source="open_meteo", temp=10),
            _obs(ts=1700003600, source="open_meteo", temp=11),
            _obs(ts=1700003600, source="knmi", station="06260", temp=12),
        ])
        rows = store.query(self.conn, since=1700003000, until=1700004000, source="open_meteo")
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["source"], "open_meteo")
        self.assertEqual(rows[0]["temp"], 11)               # payload flattened into the row

    def test_last_valid_ts_per_source(self):
        store.upsert(self.conn, [
            _obs(ts=1700000000, source="open_meteo"),
            _obs(ts=1700003600, source="open_meteo"),
        ])
        self.assertEqual(store.last_valid_ts(self.conn, "open_meteo"), 1700003600)
        self.assertIsNone(store.last_valid_ts(self.conn, "knmi"))


if __name__ == "__main__":
    unittest.main()
