"""Parse tests for the keyed providers (KNMI / Netatmo / WU).

No live keys here, so fixtures are hand-built to each API's *documented* JSON shape.
These verify our parsing logic; a live smoke test is still required once real keys
exist (the parse seam is what these lock down)."""
import unittest

from weather import knmi, netatmo, wu


class TestNetatmo(unittest.TestCase):
    def test_publicdata_station_merges_modules(self):
        raw = {"status": "ok", "body": [{
            "_id": "70:ee:50:aa",
            "place": {"location": [5.18, 52.10], "altitude": 8},
            "measures": {
                "02:00:00:aa": {"res": {"1700000000": [12.3, 81]},
                                "type": ["temperature", "humidity"]},
                "05:00:00:bb": {"res": {"1700000000": [1011.4]}, "type": ["pressure"]},
                "06:00:00:cc": {"rain_live": 0.2, "rain_60min": 0.5, "rain_24h": 2.0},
                "07:00:00:dd": {"wind_strength": 14, "wind_angle": 200,
                                "gust_strength": 25, "gust_angle": 210},
            },
        }]}
        obs = netatmo.parse_publicdata(raw, our_lat=52.18, our_lon=5.28)
        self.assertEqual(len(obs), 1)
        o = obs[0]
        self.assertEqual(o.source, "netatmo")
        self.assertEqual(o.station_id, "70:ee:50:aa")
        self.assertEqual(o.valid_ts, 1700000000)
        self.assertEqual(o.values["temperature"], 12.3)
        self.assertEqual(o.values["humidity"], 81)
        self.assertEqual(o.values["pressure"], 1011.4)
        self.assertEqual(o.values["rain_live"], 0.2)
        self.assertEqual(o.values["wind_strength"], 14)
        self.assertEqual(o.values["gust_strength"], 25)
        self.assertGreater(o.distance_km, 0)

    def test_empty_body_yields_nothing(self):
        self.assertEqual(netatmo.parse_publicdata({"body": []}, our_lat=52.18, our_lon=5.28), [])


class TestWeatherUnderground(unittest.TestCase):
    def test_current_observation_metric_block(self):
        raw = {"observations": [{
            "stationID": "IUTRECHT42", "obsTimeUtc": "2024-01-02T15:00:00Z",
            "lat": 52.11, "lon": 5.19, "humidity": 77, "winddir": 230,
            "solarRadiation": 120.0, "uv": 1.0,
            "metric": {"temp": 6, "dewpt": 3, "pressure": 1018.2,
                       "windSpeed": 12, "windGust": 20, "precipRate": 0.0, "precipTotal": 1.2},
        }]}
        obs = wu.parse_current(raw)
        self.assertEqual(len(obs), 1)
        o = obs[0]
        self.assertEqual(o.source, "wu")
        self.assertEqual(o.station_id, "IUTRECHT42")
        self.assertEqual(o.valid_ts, 1704207600)
        self.assertEqual(o.values["temp"], 6)
        self.assertEqual(o.values["pressure"], 1018.2)
        self.assertEqual(o.values["windGust"], 20)
        self.assertEqual(o.values["humidity"], 77)        # top-level fields included
        self.assertEqual(o.values["solarRadiation"], 120.0)


class TestKnmiEdr(unittest.TestCase):
    def test_coveragejson_position_to_obs_per_time(self):
        # OGC EDR / CoverageJSON position-query shape from the KNMI Data Platform.
        raw = {
            "type": "Coverage",
            "domain": {"axes": {"t": {"values": ["2024-01-02T15:00:00Z",
                                                  "2024-01-02T15:10:00Z"]}}},
            "ranges": {
                "ta": {"values": [4.1, 4.3]},      # air temp
                "pp": {"values": [1019.0, 1019.2]},  # pressure
                "ff": {"values": [3.0, 3.4]},      # wind speed
            },
        }
        obs = knmi.parse_edr(raw, station_id="06260", lat=52.10, lon=5.18,
                             our_lat=52.18, our_lon=5.28)
        self.assertEqual(len(obs), 2)
        self.assertEqual(obs[0].source, "knmi")
        self.assertEqual(obs[0].station_id, "06260")
        self.assertEqual(obs[0].valid_ts, 1704207600)
        # KNMI codes mapped to friendly names
        self.assertEqual(obs[0].values["temp"], 4.1)
        self.assertEqual(obs[1].values["pressure"], 1019.2)
        self.assertEqual(obs[0].values["wind_speed"], 3.0)
        self.assertEqual(obs[0].distance_km, obs[1].distance_km)
        self.assertGreater(obs[0].distance_km, 0)


if __name__ == "__main__":
    unittest.main()
