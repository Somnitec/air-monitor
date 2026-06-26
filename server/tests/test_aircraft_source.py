"""Tests for reading a readsb aircraft.json (file path) and normalizing it."""
import unittest
from pathlib import Path

from aircraft.base import normalize
from aircraft.source import read_source

FIXTURE = Path(__file__).parent / "fixtures" / "aircraft_sample.json"
HOME_LAT, HOME_LON = 52.179722, 5.284722   # Paleis Soestdijk placeholder


class TestSource(unittest.TestCase):
    def test_reads_and_parses_file(self):
        raw = read_source(path=str(FIXTURE))
        self.assertIsNotNone(raw)
        self.assertEqual(len(raw["aircraft"]), 6)

    def test_missing_file_returns_none(self):
        self.assertIsNone(read_source(path="/no/such/aircraft.json"))

    def test_normalize_over_fixture_drops_nopos_and_stale(self):
        out = normalize(read_source(path=str(FIXTURE)), HOME_LAT, HOME_LON,
                        stale_sec=60, max_range_km=300)
        # 6 raw -> drop the position-less one and the stale one = 4
        self.assertEqual(len(out), 4)
        self.assertTrue(all(a.lat is not None for a in out))


if __name__ == "__main__":
    unittest.main()
