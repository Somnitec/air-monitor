"""Parse tests for the keyless providers, run against real recorded API responses."""
import json
import pathlib
import unittest

from weather import open_meteo, luchtmeetnet, sensor_community

FIX = pathlib.Path(__file__).parent / "fixtures"


def load(name):
    return json.loads((FIX / name).read_text())


class TestOpenMeteoCurrent(unittest.TestCase):
    def test_weather_current_becomes_one_obs(self):
        obs = open_meteo.parse_current(load("open_meteo_weather.json"), source="open_meteo")
        self.assertEqual(len(obs), 1)
        o = obs[0]
        self.assertEqual(o.source, "open_meteo")
        self.assertEqual(o.kind, "obs")
        self.assertEqual(o.values["pressure_msl"], 1014.4)
        self.assertEqual(o.values["wind_direction_10m"], 206)
        # ISO "2026-06-26T15:30" (UTC) -> epoch
        self.assertEqual(o.valid_ts, 1782487800)
        # bookkeeping fields are not stored as variables
        self.assertNotIn("time", o.values)
        self.assertNotIn("interval", o.values)

    def test_air_quality_current_uses_its_own_source(self):
        obs = open_meteo.parse_current(load("open_meteo_aq.json"), source="open_meteo_aq")
        o = obs[0]
        self.assertEqual(o.source, "open_meteo_aq")
        self.assertEqual(o.values["pm2_5"], 15.7)
        self.assertEqual(o.values["european_aqi"], 65)

    def test_archive_hourly_becomes_reanalysis_rows(self):
        obs = open_meteo.parse_hourly(load("open_meteo_archive.json"), source="open_meteo")
        self.assertEqual(len(obs), 3)               # 3 hours in the fixture
        self.assertTrue(all(o.kind == "reanalysis" for o in obs))
        self.assertEqual(obs[0].values["temperature_2m"], 21.0)
        self.assertEqual(obs[1].values["pressure_msl"], 1017.1)
        # hours are one apart
        self.assertEqual(obs[1].valid_ts - obs[0].valid_ts, 3600)


class TestLuchtmeetnet(unittest.TestCase):
    def test_measurements_group_by_station_and_time(self):
        # Build a tiny measurement set: two formulae, same station+time -> one merged obs.
        raw = {"data": [
            {"station_number": "NL10643", "value": 13.1, "formula": "PM25",
             "timestamp_measured": "2026-06-26T15:00:00+00:00"},
            {"station_number": "NL10643", "value": 22.0, "formula": "NO2",
             "timestamp_measured": "2026-06-26T15:00:00+00:00"},
            {"station_number": "NL10643", "value": 14.0, "formula": "PM25",
             "timestamp_measured": "2026-06-26T16:00:00+00:00"},
        ]}
        coords = {"NL10643": (52.10, 5.18)}
        obs = luchtmeetnet.parse_measurements(raw, coords, our_lat=52.18, our_lon=5.28)
        # two distinct timestamps -> two obs; first merges PM25+NO2
        self.assertEqual(len(obs), 2)
        first = [o for o in obs if o.valid_ts == 1782486000][0]
        self.assertEqual(first.source, "luchtmeetnet")
        self.assertEqual(first.station_id, "NL10643")
        self.assertEqual(first.values["pm25"], 13.1)
        self.assertEqual(first.values["no2"], 22.0)
        self.assertGreater(first.distance_km, 0)        # distance computed from coords

    def test_unknown_station_coords_are_tolerated(self):
        raw = {"data": [{"station_number": "NLXXX", "value": 1.0, "formula": "O3",
                         "timestamp_measured": "2026-06-26T15:00:00+00:00"}]}
        obs = luchtmeetnet.parse_measurements(raw, {}, our_lat=52.18, our_lon=5.28)
        self.assertEqual(len(obs), 1)
        self.assertIsNone(obs[0].distance_km)


class TestSensorCommunity(unittest.TestCase):
    def test_outdoor_pm_records_parsed_with_distance(self):
        obs = sensor_community.parse(load("sensor_community.json"), our_lat=52.18, our_lon=5.28)
        self.assertTrue(obs, "expected at least one parsed record")
        o = obs[0]
        self.assertEqual(o.source, "sensor_community")
        self.assertIsNotNone(o.distance_km)
        # values come straight from sensordatavalues, numeric-coerced
        for k, v in o.values.items():
            self.assertIsInstance(v, float)

    def test_indoor_sensors_are_skipped(self):
        raw = [{
            "timestamp": "2026-06-26 15:45:01",
            "sensordatavalues": [{"value": "5", "value_type": "P2"}],
            "sensor": {"id": 1}, "location": {"indoor": 1, "latitude": "52.1", "longitude": "5.2"},
        }]
        self.assertEqual(sensor_community.parse(raw, our_lat=52.18, our_lon=5.28), [])


if __name__ == "__main__":
    unittest.main()
