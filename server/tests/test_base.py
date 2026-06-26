"""Tests for the weather provider foundation: distance + the WeatherObs value object."""
import unittest

from weather.base import WeatherObs, haversine_km


class TestHaversine(unittest.TestCase):
    def test_same_point_is_zero(self):
        self.assertAlmostEqual(haversine_km(52.18, 5.28, 52.18, 5.28), 0.0, places=6)

    def test_one_degree_longitude_at_equator(self):
        # ~111.19 km per degree along the equator (mean-radius great circle).
        self.assertAlmostEqual(haversine_km(0.0, 0.0, 0.0, 1.0), 111.19, delta=0.3)

    def test_de_bilt_from_soestdijk_is_about_8km(self):
        # Soestdijk (our default) -> KNMI De Bilt 06260. Real-world ~8 km.
        d = haversine_km(52.179722, 5.284722, 52.10, 5.18)
        self.assertGreater(d, 5)
        self.assertLess(d, 15)


class TestWeatherObs(unittest.TestCase):
    def test_holds_values_and_provenance(self):
        obs = WeatherObs(
            source="open_meteo", station_id="", kind="obs",
            valid_ts=1700000000, lat=52.18, lon=5.28, distance_km=0.0,
            values={"temp": 14.2, "pressure_msl": 1013.5},
        )
        self.assertEqual(obs.source, "open_meteo")
        self.assertEqual(obs.values["pressure_msl"], 1013.5)

    def test_dedupe_key_identifies_a_unique_slot(self):
        # The (source, station_id, valid_ts, kind) tuple is what the store dedupes on.
        obs = WeatherObs("knmi", "06260", "obs", 1700000000, 52.1, 5.18, 8.0, {"temp": 1})
        self.assertEqual(obs.dedupe_key(), ("knmi", "06260", 1700000000, "obs"))
