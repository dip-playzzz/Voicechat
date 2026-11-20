"""Deterministic relay tests for multiplayer routing and reconnects. Run: python3 test_multiplayer.py"""
import itertools
import struct
import time
import unittest
from unittest.mock import patch

import protocol as P
from server import Relay


class Transport:
    def __init__(self):
        self.sent = []

    def sendto(self, data, addr):
        self.sent.append((addr, data))

    def packets(self, addr, kind):
        return [data for target, data in self.sent if target == addr and data[3] == kind]


class MultiplayerTest(unittest.TestCase):
    def setUp(self):
        self.now = 100.0
        self.clock = patch("server.time.monotonic", side_effect=lambda: self.now)
        self.clock.start()
        self.addCleanup(self.clock.stop)
        self.relay = Relay(radius=32, secret="secret", secrets={}, max_room=4, max_total=8, log=lambda _: None)
        self.relay.transport = self.transport = Transport()
        self.nonces = itertools.count(1)

    def join(self, addr, pid, room="r"):
        data = P.pack_hello(pid, room, "secret", int(time.time()), next(self.nonces))
        self.relay.datagram_received(data, addr)
        packets = self.transport.packets(addr, P.ACCEPT)
        self.assertTrue(packets)
        return struct.unpack_from("<Q", packets[-1], 4)[0]

    def pos(self, addr, session, x=0, dim=0):
        self.relay.datagram_received(P.pack_pos(session, dim, x, 64, 0, 0), addr)

    def voice(self, addr, session, x=0, dim=0):
        self.relay.datagram_received(P.pack_voice(session, 1, dim, x, 64, 0, 0, b"opus"), addr)

    def test_routing_requires_a_recent_position_and_respects_rooms_dimensions_and_distance(self):
        a, b, c, d = [("127.0.0.1", i) for i in range(1001, 1005)]
        sa, sb, sc, sd = self.join(a, 1), self.join(b, 2), self.join(c, 3), self.join(d, 4, "other")
        self.pos(a, sa)
        self.pos(c, sc, x=100)
        self.pos(d, sd)
        self.transport.sent.clear()
        self.voice(a, sa)
        self.assertFalse(self.transport.packets(b, P.VOICE_FWD), "a newly joined listener has no position")
        self.assertFalse(self.transport.packets(c, P.VOICE_FWD), "far players cannot hear")
        self.assertFalse(self.transport.packets(d, P.VOICE_FWD), "other rooms cannot hear")

        self.pos(b, sb, x=5)
        self.voice(a, sa)
        self.assertEqual(len(self.transport.packets(b, P.VOICE_FWD)), 1)
        self.assertFalse(self.transport.packets(a, P.VOICE_FWD), "sender does not hear its own relay packet")
        self.transport.sent.clear()
        self.voice(b, sb, x=5)
        self.assertEqual(len(self.transport.packets(a, P.VOICE_FWD)), 1, "voice also flows in the other direction")

        self.pos(b, sb, x=5, dim=1)
        self.transport.sent.clear()
        self.voice(a, sa)
        self.assertFalse(self.transport.packets(b, P.VOICE_FWD), "different dimensions are isolated")

        self.pos(b, sb, x=5)
        self.now += 3.1
        # Keep-alive traffic must not make an old position fresh.
        self.relay.datagram_received(P.pack_session_only(P.PING, sb), b)
        self.transport.sent.clear()
        self.voice(a, sa)
        self.assertFalse(self.transport.packets(b, P.VOICE_FWD), "stale positions do not receive voice")
        self.pos(b, sb, x=5)
        self.voice(a, sa)
        self.assertEqual(len(self.transport.packets(b, P.VOICE_FWD)), 1, "a new position resumes voice")

    def test_reusing_a_socket_does_not_leave_a_stale_player_mapping(self):
        a, b = ("127.0.0.1", 1010), ("127.0.0.1", 1011)
        self.join(a, 1, "r")
        replacement = self.join(a, 2, "other")
        self.join(b, 1, "r")
        self.assertEqual(self.relay.clients[a].pid, 2)
        self.assertEqual(self.relay.clients[a].session, replacement)
        self.assertEqual(self.relay.by_pid[("other", 2)], a)
        self.assertEqual(self.relay.by_pid[("r", 1)], b)

    def test_switching_into_full_room_preserves_the_old_session(self):
        self.relay.max_room = 1
        a, b = ("127.0.0.1", 1020), ("127.0.0.1", 1021)
        sa = self.join(a, 1, "one")
        self.join(b, 2, "two")
        self.transport.sent.clear()
        self.relay.datagram_received(P.pack_hello(1, "two", "secret", int(time.time()), next(self.nonces)), a)
        self.assertEqual(self.transport.packets(a, P.REJECT)[-1][4], P.R_ROOM_FULL)
        self.assertEqual(self.relay.clients[a].session, sa)
        self.assertEqual(self.relay.clients[a].room, "one")
        self.assertEqual(self.relay.by_pid[("one", 1)], a)

    def test_malformed_positions_and_audio_are_dropped(self):
        self.assertIsNone(P.parse(P.pack_pos(1, 0, float("nan"), 64, 0, 0)))
        self.assertIsNone(P.parse(P.pack_pos(1, 0, 0, 64, 0, float("inf"))))
        self.assertIsNone(P.parse(P.pack_pos(1, 0, 0, 64, 0, 0) + b"extra"))
        self.assertIsNone(P.parse(P.pack_voice(1, 0, 0, 0, 64, 0, 0, b"")))
        self.assertIsNone(P.parse(P.pack_voice(1, 0, 0, 0, 64, 0, 0, b"x" * 401)))
        self.assertIsNone(P.parse(P.pack_session_only(P.PING, 1) + b"extra"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
