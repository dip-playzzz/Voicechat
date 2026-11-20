"""Proximity voice relay (protocol v2).

  python3 server.py --port 24454 --radius 32 --secret MY_ROOM_SECRET
  python3 server.py --secrets-file rooms.json          # {"room-name": "secret", ...}

Each client proves it knows the room secret in its HELLO (HMAC, replay-protected) and gets a random
64-bit session id that must accompany every later packet. Voice is only forwarded to clients in the
same room and dimension within `radius` blocks. Per-client rate limits, room/server caps and
silent drops of anything malformed keep a public server from being trivially abused.
"""
import argparse, asyncio, json, os, struct, time
import hmac as _hmac
import protocol as P

TIMEOUT = 10.0          # seconds without packets -> client dropped
HELLO_WINDOW = 60       # allowed clock skew, seconds
RATE_PPS, RATE_BURST = 200.0, 400.0


class Client:
    __slots__ = ("pid", "room", "session", "dim", "pos", "last", "tokens", "tstamp")

    def __init__(self, pid, room, session):
        now = time.monotonic()
        self.pid, self.room, self.session = pid, room, session
        self.dim, self.pos, self.last = 0, (0.0, 0.0, 0.0), now
        self.tokens, self.tstamp = RATE_BURST, now


class Relay(asyncio.DatagramProtocol):
    def __init__(self, radius, secret, secrets, max_room, max_total, log=print):
        self.radius, self.secret, self.secrets = radius, secret, secrets
        self.max_room, self.max_total, self.log = max_room, max_total, log
        self.clients = {}      # addr -> Client
        self.by_pid = {}       # (room, pid) -> addr
        self.seen = {}         # (pid, nonce) -> expiry   (HELLO replay cache)
        self.ip_hello = {}     # ip -> [window_start, count]
        self.ip_reject = {}    # ip -> [window_start, count]
        self.stats = dict(voice_in=0, voice_out=0, dropped=0)

    # ---- plumbing
    def connection_made(self, transport):
        self.transport = transport
        asyncio.get_event_loop().create_task(self.reaper())

    async def reaper(self):
        while True:
            await asyncio.sleep(2)
            now, wall = time.monotonic(), time.time()
            for a in [a for a, c in self.clients.items() if now - c.last > TIMEOUT]:
                self.drop(a)
            for k in [k for k, e in self.seen.items() if e < wall]:
                del self.seen[k]
            for d in (self.ip_hello, self.ip_reject):
                for ip in [i for i, (t, _) in d.items() if now - t > 30]:
                    del d[ip]

    def drop(self, addr):
        c = self.clients.pop(addr, None)
        if c and self.by_pid.get((c.room, c.pid)) == addr:
            del self.by_pid[(c.room, c.pid)]

    def _limited(self, table, ip, limit, window=10.0):
        now = time.monotonic()
        t = table.get(ip)
        if not t or now - t[0] > window:
            table[ip] = [now, 1]
            return False
        t[1] += 1
        return t[1] > limit

    def _reject(self, addr, reason):
        if self._limited(self.ip_reject, addr[0], 20):
            return
        self.transport.sendto(P.header(P.REJECT) + struct.pack("<BI", reason, int(time.time())), addr)

    # ---- packets
    def datagram_received(self, data, addr):
        msg = P.parse(data)
        if msg is None:
            self.stats["dropped"] += 1
            return
        typ, f = msg
        if typ == P.HELLO:
            return self.on_hello(f, addr)

        c = self.clients.get(addr)
        if c is None or not _hmac.compare_digest(struct.pack("<Q", f["session"]), struct.pack("<Q", c.session)):
            return self._reject(addr, P.R_UNKNOWN_SESSION)   # tells a restarted-server client to re-HELLO
        now = time.monotonic()
        c.tokens = min(RATE_BURST, c.tokens + (now - c.tstamp) * RATE_PPS)
        c.tstamp = now
        if c.tokens < 1:
            self.stats["dropped"] += 1
            return
        c.tokens -= 1
        c.last = now

        if typ == P.BYE:
            self.drop(addr)
        elif typ == P.PING:
            self.transport.sendto(P.header(P.PONG), addr)
        elif typ == P.POS:
            c.dim, c.pos = f["dim"], f["pos"]
        elif typ == P.VOICE:
            c.dim, c.pos = f["dim"], f["pos"]
            self.stats["voice_in"] += 1
            self.forward(addr, c, f)

    def on_hello(self, f, addr):
        if self._limited(self.ip_hello, addr[0], 30):
            return                                   # silent drop: don't help floods
        room, pid = f["room"], f["player_id"]
        if not (0 < len(room) <= 64):
            return self._reject(addr, P.R_BAD_HELLO)
        secret = self.secrets.get(room, self.secret)
        if not secret:
            return self._reject(addr, P.R_BAD_MAC)
        want = P.hello_mac(secret, pid, room, f["ts"], f["nonce"])
        if not _hmac.compare_digest(want, f["mac"]):
            return self._reject(addr, P.R_BAD_MAC)
        wall = time.time()
        if abs(wall - f["ts"]) > HELLO_WINDOW or (pid, f["nonce"]) in self.seen:
            return self._reject(addr, P.R_STALE)
        self.seen[(pid, f["nonce"])] = wall + 2 * HELLO_WINDOW

        old = self.by_pid.get((room, pid))
        if old is not None and old != addr:
            self.drop(old)                           # same player reconnecting from a new address
        in_room = sum(1 for c in self.clients.values() if c.room == room)
        if addr not in self.clients and (in_room >= self.max_room):
            return self._reject(addr, P.R_ROOM_FULL)
        if addr not in self.clients and len(self.clients) >= self.max_total:
            return self._reject(addr, P.R_SERVER_FULL)

        session = struct.unpack("<Q", os.urandom(8))[0]
        self.clients[addr] = Client(pid, room, session)
        self.by_pid[(room, pid)] = addr
        self.log(f"[+] player {pid} joined room '{room}' from {addr[0]}")
        self.transport.sendto(P.header(P.ACCEPT) + struct.pack("<Q", session), addr)

    def forward(self, src_addr, src, f):
        pkt = (P.header(P.VOICE_FWD)
               + P.FWD_HEAD.pack(src.pid, f["seq"], f["dim"], *f["pos"], f["yaw"])
               + struct.pack("<H", len(f["opus"])) + f["opus"])
        r2 = self.radius * self.radius
        for addr, c in self.clients.items():
            if addr == src_addr or c.room != src.room or c.dim != src.dim:
                continue
            dx, dy, dz = (c.pos[i] - src.pos[i] for i in range(3))
            if dx * dx + dy * dy + dz * dz <= r2:
                self.transport.sendto(pkt, addr)
                self.stats["voice_out"] += 1


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=24454)
    ap.add_argument("--radius", type=float, default=32.0)
    ap.add_argument("--secret", default=os.environ.get("VC_SECRET", ""),
                    help="secret for every room (or set VC_SECRET)")
    ap.add_argument("--secrets-file", help='JSON {"room": "secret"}; overrides --secret per room')
    ap.add_argument("--max-room", type=int, default=32)
    ap.add_argument("--max-total", type=int, default=512)
    a = ap.parse_args()
    secrets = json.load(open(a.secrets_file)) if a.secrets_file else {}
    if not a.secret and not secrets:
        raise SystemExit("refusing to start without a secret: use --secret or --secrets-file")
    loop = asyncio.get_running_loop()
    await loop.create_datagram_endpoint(
        lambda: Relay(a.radius, a.secret, secrets, a.max_room, a.max_total),
        local_addr=(a.host, a.port))
    print(f"voice relay on {a.host}:{a.port} (UDP), radius {a.radius} blocks", flush=True)
    await asyncio.Event().wait()


if __name__ == "__main__":
    asyncio.run(main())
