"""Tests for the aircraft SQLite store: schema, append-only logging, queries."""
import sqlite3
import unittest

from aircraft.base import Aircraft
from aircraft import store


def _ac(hex="abc", **kw):
    base = dict(hex=hex, flight="KLM1", type="B738", reg="PH-AAA",
                lat=52.0, lon=5.0, alt_baro=2000, gs=250.0, track=120.0,
                baro_rate=-640, distance_km=3.2, bearing_deg=90.0,
                rssi=-12.0, seen=0.5)
    base.update(kw)
    return Aircraft(**base)


class TestAircraftStore(unittest.TestCase):
    def setUp(self):
        self.conn = sqlite3.connect(":memory:")
        self.conn.row_factory = sqlite3.Row
        store.init_aircraft_table(self.conn)

    def _count(self):
        return self.conn.execute("SELECT COUNT(*) FROM aircraft").fetchone()[0]

    def test_insert_one(self):
        n = store.insert(self.conn, [_ac()], now=1000)
        self.assertEqual(n, 1)
        self.assertEqual(self._count(), 1)

    def test_stores_fields_and_sample_time(self):
        store.insert(self.conn, [_ac(hex="4ca", type="A320")], now=1700000000)
        row = self.conn.execute("SELECT * FROM aircraft").fetchone()
        self.assertEqual(row["hex"], "4ca")
        self.assertEqual(row["type"], "A320")
        self.assertEqual(row["ts"], 1700000000)
        self.assertEqual(row["distance_km"], 3.2)

    def test_append_only_same_hex_makes_new_rows(self):
        store.insert(self.conn, [_ac(hex="x")], now=1000)
        store.insert(self.conn, [_ac(hex="x")], now=1015)
        self.assertEqual(self._count(), 2)   # time-series logging, not dedup

    def test_query_by_hex(self):
        store.insert(self.conn, [_ac(hex="a"), _ac(hex="b")], now=1000)
        store.insert(self.conn, [_ac(hex="a")], now=2000)
        self.assertEqual(len(store.query(self.conn, hex="a")), 2)

    def test_query_by_time_range(self):
        store.insert(self.conn, [_ac(hex="a")], now=1000)
        store.insert(self.conn, [_ac(hex="a")], now=2000)
        self.assertEqual(len(store.query(self.conn, since=1500)), 1)


if __name__ == "__main__":
    unittest.main()
