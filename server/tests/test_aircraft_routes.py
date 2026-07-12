"""Tests for the route cache/backfill: adsbdb parsing, persistence, freshness, and the
retroactive backfill sweep. Network is stubbed — no real adsbdb calls."""
import asyncio
import sqlite3
import unittest
from unittest.mock import patch

from aircraft import routes, store


# A trimmed adsbdb response for KLM1234 (CDG -> AMS).
ADSBDB_OK = {
    "response": {"flightroute": {
        "callsign": "KLM1234",
        "airline": {"name": "KLM Royal Dutch Airlines"},
        "origin": {"icao_code": "LFPG", "iata_code": "CDG",
                   "municipality": "Paris", "name": "Charles de Gaulle"},
        "destination": {"icao_code": "EHAM", "iata_code": "AMS",
                        "municipality": "Amsterdam", "name": "Schiphol"},
    }}
}


class TestParse(unittest.TestCase):
    def test_parses_origin_destination(self):
        r = routes._parse(ADSBDB_OK)
        self.assertEqual(r["origin_city"], "Paris")
        self.assertEqual(r["origin_icao"], "LFPG")
        self.assertEqual(r["dest_city"], "Amsterdam")
        self.assertEqual(r["dest_iata"], "AMS")
        self.assertEqual(r["airline"], "KLM Royal Dutch Airlines")

    def test_no_flightroute_is_none(self):
        self.assertIsNone(routes._parse({"response": {"flightroute": None}}))
        self.assertIsNone(routes._parse({"response": "unknown callsign"}))
        self.assertIsNone(routes._parse({}))

    def test_empty_endpoints_is_none(self):
        self.assertIsNone(routes._parse(
            {"response": {"flightroute": {"origin": {}, "destination": {}}}}))


class TestPersistence(unittest.TestCase):
    def setUp(self):
        self.conn = sqlite3.connect(":memory:", check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        store.init_aircraft_table(self.conn)
        routes.init_routes_table(self.conn)

    def test_upsert_and_get_roundtrip(self):
        routes.upsert(self.conn, "KLM1234", routes._parse(ADSBDB_OK), now=1000)
        row = routes.get(self.conn, "KLM1234")
        self.assertEqual(row["found"], 1)
        self.assertEqual(row["origin_city"], "Paris")
        self.assertEqual(row["dest_city"], "Amsterdam")

    def test_negative_cache_roundtrip(self):
        routes.upsert(self.conn, "NOROUTE1", None, now=1000)
        row = routes.get(self.conn, "NOROUTE1")
        self.assertEqual(row["found"], 0)
        self.assertIsNone(row["origin_city"])

    def test_upsert_overwrites(self):
        routes.upsert(self.conn, "KLM1234", None, now=1000)              # miss first
        routes.upsert(self.conn, "KLM1234", routes._parse(ADSBDB_OK), now=2000)
        row = routes.get(self.conn, "KLM1234")
        self.assertEqual(row["found"], 1)
        self.assertEqual(row["fetched_at"], 2000)

    def test_is_fresh(self):
        hit = {"found": 1, "fetched_at": 0}
        self.assertTrue(routes.is_fresh(hit, now=10**9))                 # hits never expire
        recent_miss = {"found": 0, "fetched_at": 1000}
        self.assertTrue(routes.is_fresh(recent_miss, now=1000 + 10))
        stale_miss = {"found": 0, "fetched_at": 1000}
        self.assertFalse(routes.is_fresh(stale_miss, now=1000 + routes.NEG_CACHE_TTL + 1))


def _log_aircraft(conn, flight):
    """Log one sighting with the given callsign so it becomes backfill work."""
    from aircraft.base import Aircraft
    ac = Aircraft(hex="abc", flight=flight, type="B738", reg=None, lat=52.0, lon=5.0,
                  alt_baro=3000, gs=None, track=None, baro_rate=None,
                  distance_km=3.0, bearing_deg=0.0, rssi=None, seen=1.0)
    store.insert(conn, [ac], now=1000)


class TestBackfill(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.conn = sqlite3.connect(":memory:", check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        store.init_aircraft_table(self.conn)
        routes.init_routes_table(self.conn)
        self.lock = asyncio.Lock()

    def test_missing_callsigns_lists_unresolved(self):
        _log_aircraft(self.conn, "KLM1234")
        _log_aircraft(self.conn, "")          # blank -> ignored
        missing = routes.missing_callsigns(self.conn, limit=10, now=1000)
        self.assertEqual(missing, ["KLM1234"])

    def test_missing_skips_already_resolved(self):
        _log_aircraft(self.conn, "KLM1234")
        routes.upsert(self.conn, "KLM1234", routes._parse(ADSBDB_OK), now=1000)
        self.assertEqual(routes.missing_callsigns(self.conn, limit=10, now=1001), [])

    def test_missing_retries_stale_negative(self):
        _log_aircraft(self.conn, "KLM1234")
        routes.upsert(self.conn, "KLM1234", None, now=1000)             # negative cache
        fresh = routes.missing_callsigns(self.conn, limit=10, now=1000 + 100)
        self.assertEqual(fresh, [])                                     # still fresh miss
        stale = routes.missing_callsigns(self.conn, limit=10,
                                         now=1000 + routes.NEG_CACHE_TTL + 1)
        self.assertEqual(stale, ["KLM1234"])                           # retry-able

    async def test_backfill_resolves_and_persists(self):
        _log_aircraft(self.conn, "KLM1234")
        with patch.object(routes, "fetch", return_value=routes._parse(ADSBDB_OK)):
            n = await routes.backfill_once(self.conn, self.lock, pace_sec=0, now=2000)
        self.assertEqual(n, 1)
        self.assertEqual(routes.get(self.conn, "KLM1234")["origin_city"], "Paris")

    async def test_backfill_stops_on_network_error(self):
        _log_aircraft(self.conn, "KLM1234")
        with patch.object(routes, "fetch", side_effect=OSError("offline")):
            n = await routes.backfill_once(self.conn, self.lock, pace_sec=0, now=2000)
        self.assertEqual(n, 0)                          # nothing resolved
        self.assertIsNone(routes.get(self.conn, "KLM1234"))   # not even negatively cached

    async def test_backfill_negatively_caches_unknown(self):
        _log_aircraft(self.conn, "GA0001")
        with patch.object(routes, "fetch", return_value=None):     # adsbdb 404
            n = await routes.backfill_once(self.conn, self.lock, pace_sec=0, now=2000)
        self.assertEqual(n, 1)
        self.assertEqual(routes.get(self.conn, "GA0001")["found"], 0)


if __name__ == "__main__":
    unittest.main()
