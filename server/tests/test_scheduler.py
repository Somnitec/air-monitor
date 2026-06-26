"""Scheduler logic: concurrent fan-out resilience + gap-backfill windowing."""
import unittest

from weather.base import Provider, WeatherObs
from weather import scheduler


def _obs(source, ts=1700000000):
    return WeatherObs(source, "", "obs", ts, 52.18, 5.28, 0.0, {"temp": 1.0})


class _OK(Provider):
    def __init__(self, name): self.name = name
    async def fetch(self, since=None, until=None): return [_obs(self.name)]


class _Boom(Provider):
    name = "boom"
    async def fetch(self, since=None, until=None): raise RuntimeError("api down")


class _Disabled(Provider):
    name = "off"
    def enabled(self): return False
    async def fetch(self, since=None, until=None): return [_obs("off")]


class TestPollResilience(unittest.IsolatedAsyncioTestCase):
    async def test_one_failing_provider_does_not_block_others(self):
        obs, errors = await scheduler.poll_providers([_OK("a"), _Boom(), _OK("b")])
        sources = sorted(o.source for o in obs)
        self.assertEqual(sources, ["a", "b"])
        self.assertEqual([name for name, _ in errors], ["boom"])

    async def test_disabled_providers_are_skipped(self):
        obs, errors = await scheduler.poll_providers([_OK("a"), _Disabled()])
        self.assertEqual([o.source for o in obs], ["a"])
        self.assertEqual(errors, [])


class TestBackfillWindow(unittest.TestCase):
    def test_no_history_returns_a_capped_window(self):
        win = scheduler.backfill_window(None, now=1_000_000, threshold_s=7200, max_days=7)
        self.assertIsNotNone(win)
        since, until = win
        self.assertEqual(until, 1_000_000)
        self.assertEqual(since, 1_000_000 - 7 * 86400)      # capped at max_days

    def test_small_gap_needs_no_backfill(self):
        # last point 1 h ago, threshold 2 h -> nothing to do
        self.assertIsNone(scheduler.backfill_window(1_000_000 - 3600, now=1_000_000,
                                                    threshold_s=7200, max_days=7))

    def test_large_gap_backfills_from_last_point(self):
        last = 1_000_000 - 3 * 86400
        win = scheduler.backfill_window(last, now=1_000_000, threshold_s=7200, max_days=7)
        self.assertEqual(win, (last, 1_000_000))


if __name__ == "__main__":
    unittest.main()
