"""Tests for shared geo helpers: great-circle distance + initial bearing."""
import unittest

from geo import haversine_km, bearing_deg


class TestHaversine(unittest.TestCase):
    def test_same_point_is_zero(self):
        self.assertAlmostEqual(haversine_km(52.18, 5.28, 52.18, 5.28), 0.0, places=6)

    def test_one_degree_longitude_at_equator(self):
        # ~111.19 km per degree along the equator (mean-radius great circle).
        self.assertAlmostEqual(haversine_km(0.0, 0.0, 0.0, 1.0), 111.19, delta=0.3)


class TestBearing(unittest.TestCase):
    def test_due_north(self):
        self.assertAlmostEqual(bearing_deg(0.0, 0.0, 1.0, 0.0), 0.0, delta=0.5)

    def test_due_east(self):
        self.assertAlmostEqual(bearing_deg(0.0, 0.0, 0.0, 1.0), 90.0, delta=0.5)

    def test_due_south(self):
        self.assertAlmostEqual(bearing_deg(0.0, 0.0, -1.0, 0.0), 180.0, delta=0.5)

    def test_due_west(self):
        self.assertAlmostEqual(bearing_deg(0.0, 0.0, 0.0, -1.0), 270.0, delta=0.5)

    def test_always_in_0_to_360(self):
        b = bearing_deg(52.0, 4.0, 51.0, 5.0)   # roughly south-east
        self.assertGreaterEqual(b, 0.0)
        self.assertLess(b, 360.0)


if __name__ == "__main__":
    unittest.main()
