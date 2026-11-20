"""Voice relay wire format, version 2. All integers little-endian.

Header (4 bytes): 'V' 'C' | version u8 (=2) | type u8

client -> server
  HELLO (1): player_id u32 | room_len u8 | room utf8 | ts u32 | nonce u64 | mac 16 bytes
             mac = HMAC-SHA256(secret, "VC2HELLO" | player_id u32 | room | ts u32 | nonce u64)[:16]
  POS   (3): session u64 | dim i32 | x f32 | y f32 | z f32 | yaw f32
  VOICE (2): session u64 | seq u16 | dim i32 | x y z yaw f32 | opus_len u16 | opus bytes
  PING  (8): session u64
  BYE   (4): session u64
server -> client
  ACCEPT (5): session u64
  REJECT (6): reason u8 | server_ts u32
  VOICE_FWD (7): sender_id u32 | seq u16 | dim i32 | x y z yaw f32 | opus_len u16 | opus bytes
                 (the sender's session is NEVER forwarded)
  PONG (9): (empty)

POS is sent ~10x/s and doubles as keepalive. A client behind NAT only ever sends first,
so no port forwarding is needed on the clients, only on the server.
NOTE: voice and positions are NOT encrypted yet; the secret never crosses the wire, but audio does.
"""
import hashlib
import hmac
import math
import struct

MAGIC = b"VC"
VERSION = 2
HELLO, VOICE, POS, BYE, ACCEPT, REJECT, VOICE_FWD, PING, PONG = 1, 2, 3, 4, 5, 6, 7, 8, 9
MAX_PACKET = 1200

R_BAD_MAC, R_STALE, R_ROOM_FULL, R_RATE, R_SERVER_FULL, R_BAD_HELLO, R_UNKNOWN_SESSION = 1, 2, 3, 4, 5, 6, 7
REASONS = {R_BAD_MAC: "wrong room secret", R_STALE: "clock off or replayed hello", R_ROOM_FULL: "room full",
           R_RATE: "rate limited", R_SERVER_FULL: "server full", R_BAD_HELLO: "malformed hello",
           R_UNKNOWN_SESSION: "unknown session (reconnect)"}

HDR = struct.Struct("<2sBB")
POS_BODY = struct.Struct("<Qiffff")
VOICE_HEAD = struct.Struct("<QHiffff")
FWD_HEAD = struct.Struct("<IHiffff")


def header(t):
    return HDR.pack(MAGIC, VERSION, t)


def hello_mac(secret, player_id, room, ts, nonce):
    msg = b"VC2HELLO" + struct.pack("<I", player_id) + room.encode() + struct.pack("<IQ", ts, nonce)
    return hmac.new(secret.encode(), msg, hashlib.sha256).digest()[:16]


def pack_hello(player_id, room, secret, ts, nonce):
    r = room.encode()
    return (header(HELLO) + struct.pack("<IB", player_id, len(r)) + r + struct.pack("<IQ", ts, nonce)
            + hello_mac(secret, player_id, room, ts, nonce))


def pack_pos(session, dim, x, y, z, yaw):
    return header(POS) + POS_BODY.pack(session, dim, x, y, z, yaw)


def pack_voice(session, seq, dim, x, y, z, yaw, opus):
    return header(VOICE) + VOICE_HEAD.pack(session, seq, dim, x, y, z, yaw) + struct.pack("<H", len(opus)) + opus


def pack_session_only(t, session):
    return header(t) + struct.pack("<Q", session)


def parse(data):
    """-> (type, dict) or None. Never raises on garbage."""
    try:
        if not (HDR.size <= len(data) <= MAX_PACKET):
            return None
        magic, ver, t = HDR.unpack_from(data, 0)
        if magic != MAGIC or ver != VERSION:
            return None
        o = HDR.size
        if t == HELLO:
            pid, rl = struct.unpack_from("<IB", data, o); o += 5
            room = data[o:o + rl].decode(); o += rl
            ts, nonce = struct.unpack_from("<IQ", data, o); o += 12
            mac = data[o:o + 16]
            if len(mac) != 16 or o + 16 != len(data):
                return None
            return t, dict(player_id=pid, room=room, ts=ts, nonce=nonce, mac=mac)
        if t == POS:
            if len(data) != o + POS_BODY.size:
                return None
            s, dim, x, y, z, yaw = POS_BODY.unpack_from(data, o)
            if not all(math.isfinite(v) for v in (x, y, z, yaw)):
                return None
            return t, dict(session=s, dim=dim, pos=(x, y, z), yaw=yaw)
        if t == VOICE:
            s, seq, dim, x, y, z, yaw = VOICE_HEAD.unpack_from(data, o); o += VOICE_HEAD.size
            (n,) = struct.unpack_from("<H", data, o); o += 2
            if not 0 < n <= 400 or o + n != len(data) or not all(math.isfinite(v) for v in (x, y, z, yaw)):
                return None
            return t, dict(session=s, seq=seq, dim=dim, pos=(x, y, z), yaw=yaw, opus=data[o:o + n])
        if t in (PING, BYE):
            if len(data) != o + 8:
                return None
            (s,) = struct.unpack_from("<Q", data, o)
            return t, dict(session=s)
    except (struct.error, UnicodeDecodeError, IndexError):
        return None
    return None
