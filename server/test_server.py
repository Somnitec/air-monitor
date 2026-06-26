"""Tests for the envelope ingest + testing-mode command channel.

Run:  cd server && AIRMON_DB=:memory: pytest test_server.py -v
(requires: pip install pytest httpx)
"""
import importlib
import os

os.environ["AIRMON_DB"] = ":memory:"     # don't touch the real db

import pytest
from fastapi.testclient import TestClient
import server as srv

client = TestClient(srv.app)


@pytest.fixture(autouse=True)
def _reset_device_state():
    # The DEVICE_STATE/PENDING_CMD module globals persist across tests; clear
    # them so each test starts from a clean slate (production behavior unchanged).
    srv.DEVICE_STATE.clear()
    srv.PENDING_CMD.clear()
    yield


def _envelope(mode="normal", buffered=3):
    return {
        "dev": "air-monitor-01", "boot": 999, "fw": "phase1",
        "mode": mode, "buffered": buffered,
        "records": [
            {"ts": 1719400000, "ts_ok": True, "dev": "air-monitor-01",
             "up_ms": 1000, "boot": 999, "pm25": 4.2, "co2": 800},
        ],
    }


def test_envelope_ingest_stores_record_and_device_state():
    with TestClient(srv.app) as c:
        r = c.post("/ingest", json=_envelope())
        assert r.status_code == 200
        assert r.json()["stored"] == 1
        devs = c.get("/api/devices").json()
        assert any(d["dev"] == "air-monitor-01" and d["mode"] == "normal" for d in devs)


def test_legacy_list_still_accepted():
    with TestClient(srv.app) as c:
        r = c.post("/ingest", json=[{"ts": 1719400100, "dev": "old", "up_ms": 1, "boot": 1}])
        assert r.status_code == 200
        assert r.json()["received"] == 1


def test_pending_command_returned_then_cleared():
    with TestClient(srv.app) as c:
        c.post("/ingest", json=_envelope(mode="normal"))         # device known
        c.post("/api/device/air-monitor-01/mode", json={"mode": "testing"})
        # next contact (still normal) must receive the command
        r = c.post("/ingest", json=_envelope(mode="normal"))
        assert r.json().get("command", {}).get("set_mode") == "testing"
        # once the device echoes testing, the command clears
        c.post("/ingest", json=_envelope(mode="testing"))
        r = c.post("/ingest", json=_envelope(mode="testing"))
        assert "command" not in r.json()


def test_invalid_mode_rejected():
    with TestClient(srv.app) as c:
        c.post("/ingest", json=_envelope())
        r = c.post("/api/device/air-monitor-01/mode", json={"mode": "bogus"})
        assert r.status_code == 400
