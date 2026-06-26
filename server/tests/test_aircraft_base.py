"""Tests for aircraft normalization (readsb aircraft.json -> Aircraft records)
and the per-hex log throttle. All pure; no hardware, no network."""
import unittest

from aircraft.base import normalize, select_for_logging


HOME_LAT, HOME_LON = 52.0, 5.0


def raw(aircraft):
    return {"now": 1719400000.0, "aircraft": aircraft}


class TestNormalize(unittest.TestCase):
    def test_skips_aircraft_without_position(self):
        out = normalize(raw([{"hex": "abc123", "seen": 1.0}]), HOME_LAT, HOME_LON)
        self.assertEqual(out, [])

    def test_computes_distance_and_bearing(self):
        # ~1 degree north of home -> ~111 km, bearing ~0 (due north)
        out = normalize(raw([{"hex": "abc", "lat": 53.0, "lon": 5.0, "seen": 1.0}]),
                        HOME_LAT, HOME_LON)
        self.assertEqual(len(out), 1)
        self.assertAlmostEqual(out[0].distance_km, 111.19, delta=1.0)
        self.assertAlmostEqual(out[0].bearing_deg, 0.0, delta=0.5)

    def test_drops_stale_aircraft(self):
        out = normalize(raw([{"hex": "abc", "lat": 52.01, "lon": 5.0, "seen": 120.0}]),
                        HOME_LAT, HOME_LON, stale_sec=60)
        self.assertEqual(out, [])

    def test_drops_beyond_max_range(self):
        out = normalize(raw([{"hex": "abc", "lat": 53.0, "lon": 5.0, "seen": 1.0}]),
                        HOME_LAT, HOME_LON, max_range_km=50)
        self.assertEqual(out, [])

    def test_normalizes_fields(self):
        out = normalize(raw([{
            "hex": "4ca", "flight": "KLM31G  ", "t": "B738", "r": "PH-BXA",
            "lat": 52.01, "lon": 5.0, "alt_baro": "ground", "gs": 12.5,
            "track": 270, "baro_rate": -640, "rssi": -12.3, "seen": 0.3,
        }]), HOME_LAT, HOME_LON)
        ac = out[0]
        self.assertEqual(ac.hex, "4ca")
        self.assertEqual(ac.flight, "KLM31G")     # trailing spaces stripped
        self.assertEqual(ac.type, "B738")
        self.assertEqual(ac.reg, "PH-BXA")
        self.assertEqual(ac.alt_baro, 0)          # "ground" -> 0
        self.assertEqual(ac.gs, 12.5)

    def test_sorted_nearest_first(self):
        out = normalize(raw([
            {"hex": "far",  "lat": 52.5,  "lon": 5.0, "seen": 1.0},
            {"hex": "near", "lat": 52.01, "lon": 5.0, "seen": 1.0},
        ]), HOME_LAT, HOME_LON)
        self.assertEqual([a.hex for a in out], ["near", "far"])

    def test_as_dict_is_json_friendly(self):
        ac = normalize(raw([{"hex": "abc", "lat": 52.01, "lon": 5.0, "seen": 1.0}]),
                       HOME_LAT, HOME_LON)[0]
        d = ac.as_dict()
        self.assertEqual(d["hex"], "abc")
        self.assertIn("distance_km", d)
        self.assertIn("bearing_deg", d)


class TestLogThrottle(unittest.TestCase):
    def _recs(self):
        return normalize(raw([
            {"hex": "aaa", "lat": 52.01, "lon": 5.0, "seen": 1.0},
            {"hex": "bbb", "lat": 52.02, "lon": 5.0, "seen": 1.0},
        ]), HOME_LAT, HOME_LON)

    def test_first_sighting_is_due(self):
        last = {}
        due = select_for_logging(self._recs(), last, now=1000, interval_sec=15)
        self.assertEqual({a.hex for a in due}, {"aaa", "bbb"})

    def test_within_interval_not_due_again(self):
        last = {}
        select_for_logging(self._recs(), last, now=1000, interval_sec=15)
        due = select_for_logging(self._recs(), last, now=1005, interval_sec=15)
        self.assertEqual(due, [])

    def test_due_again_after_interval(self):
        last = {}
        select_for_logging(self._recs(), last, now=1000, interval_sec=15)
        due = select_for_logging(self._recs(), last, now=1020, interval_sec=15)
        self.assertEqual({a.hex for a in due}, {"aaa", "bbb"})


if __name__ == "__main__":
    unittest.main()
