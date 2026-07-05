"""_run_mdns robustness: never advertise loopback, wait for a real LAN IP, and
re-advertise when the address changes (the boot-race + DHCP-renewal bugs that sent
the ESP32 to airmon-server.local -> 127.0.0.1 and it could never deliver readings)."""
import asyncio
import socket
import unittest
from unittest.mock import patch

import server


class _FakeAioZC:
    """Records what got (un)registered without touching the real network."""
    def __init__(self):
        self.registered, self.updated, self.unregistered = [], [], []
        self.closed = False

    async def async_register_service(self, info):
        self.registered.append(info)

    async def async_update_service(self, info):
        self.updated.append(info)

    async def async_unregister_service(self, info):
        self.unregistered.append(info)

    async def async_close(self):
        self.closed = True


def _addrs(info):
    return [socket.inet_ntoa(a) for a in info.addresses]


class TestRunMdns(unittest.IsolatedAsyncioTestCase):
    async def _drive(self, ip_sequence):
        """Run _run_mdns while _local_ips() yields ip_sequence, one value per loop.
        Once the sequence is exhausted we set the stop event so the task exits."""
        stop = asyncio.Event()
        fake = _FakeAioZC()
        seq = list(ip_sequence)
        state = {"n": 0}

        def fake_local_ips():
            i = state["n"]
            state["n"] += 1
            if i >= len(seq):
                stop.set()
                return seq[-1] if seq else []
            return seq[i]

        with patch("server._local_ips", side_effect=fake_local_ips), \
             patch("zeroconf.asyncio.AsyncZeroconf", return_value=fake), \
             patch.object(server, "_MDNS_WAIT_SEC", 0.001), \
             patch.object(server, "_MDNS_RECHECK_SEC", 0.001):
            await asyncio.wait_for(server._run_mdns(stop), timeout=5)
        return fake

    async def test_waits_for_real_ip_and_never_advertises_loopback(self):
        # Offline at boot (no LAN address yet), then WiFi comes up.
        fake = await self._drive([[], [], ["192.168.1.5"]])
        self.assertEqual(len(fake.registered), 1)
        self.assertEqual(_addrs(fake.registered[0]), ["192.168.1.5"])
        self.assertEqual(fake.updated, [])
        for info in fake.registered:
            self.assertNotIn("127.0.0.1", _addrs(info))

    async def test_readvertises_on_ip_change(self):
        # Same IP twice (no-op), then a DHCP renewal moves us to a new address.
        fake = await self._drive([["192.168.1.5"], ["192.168.1.5"], ["192.168.1.9"]])
        self.assertEqual(len(fake.registered), 1)
        self.assertEqual(_addrs(fake.registered[0]), ["192.168.1.5"])
        self.assertEqual(len(fake.updated), 1)
        self.assertEqual(_addrs(fake.updated[0]), ["192.168.1.9"])

    async def test_unregisters_and_closes_on_stop(self):
        fake = await self._drive([["192.168.1.5"]])
        self.assertEqual(len(fake.unregistered), 1)
        self.assertTrue(fake.closed)

    async def test_offline_whole_time_advertises_nothing(self):
        fake = await self._drive([[], [], []])
        self.assertEqual(fake.registered, [])
        self.assertEqual(fake.updated, [])
        self.assertEqual(fake.unregistered, [])   # nothing to unregister
        self.assertTrue(fake.closed)              # but the zeroconf handle is closed


if __name__ == "__main__":
    unittest.main()
