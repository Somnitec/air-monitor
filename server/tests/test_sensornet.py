"""Sensornet provider: pure parse/downsample logic (no network)."""
import math

from weather.sensornet import parse_ranges, energy_mean_db, build_obs, POSTS


def test_parse_ranges_drops_nulls_and_converts_ms():
    raw = {"ranges": [
        {"id": "107383", "data": [[1784015138000, None], [1784015140000, 51.9], [1784015145000, 49.7]]},
        {"id": "bogus", "data": [[1784015140000, 1.0]]},   # non-numeric id ignored
    ]}
    out = parse_ranges(raw)
    assert out[107383] == [(1784015140, 51.9), (1784015145, 49.7)]
    assert len(out) == 1


def test_energy_mean_is_not_arithmetic():
    # [40, 50] must combine to ~47.4 dB, not 45 — Leq math, not averaging.
    v = energy_mean_db([40.0, 50.0])
    assert math.isclose(v, 47.4, abs_tol=0.05)


def test_build_obs_minute_buckets_and_namespacing():
    base = 1784015100  # a whole minute
    cc = POSTS["cc"][3]
    ranges = {
        cc["laeq"]:  [(base + 5, 40.0), (base + 10, 50.0), (base + 65, 44.0)],
        cc["lamax"]: [(base + 5, 55.0), (base + 10, 61.0)],
    }
    obs = build_obs(ranges, 52.32009, 4.87947)
    cc_rows = [o for o in obs if o.station_id == "UT035"]
    assert [o.valid_ts for o in cc_rows] == [base, base + 60]
    first = cc_rows[0].values
    assert math.isclose(first["laeq_cc"], 47.4, abs_tol=0.05)   # energy mean
    assert first["lamax_cc"] == 61.0                            # max
    assert cc_rows[1].values == {"laeq_cc": 44.0}               # no lamax that minute
    # provenance: source/kind/distance
    assert cc_rows[0].source == "sensornet" and cc_rows[0].kind == "noise"
    assert 1.0 < cc_rows[0].distance_km < 1.4                   # ~1.16 km west of us
