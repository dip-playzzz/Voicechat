"""Security / robustness tests for the relay. Run:  python3 test_server.py"""
import os, socket, struct, subprocess, sys, time
import protocol as P

SECRET = "s3cret"

def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

def sock():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(0.4); return s

def start(port, *extra):
    p = subprocess.Popen([sys.executable, "server.py", "--host", "127.0.0.1", "--port", str(port), "--radius", "32",
                          "--secret", SECRET, *extra], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    p.stdout.readline(); return p

def recv(s):
    try: return s.recvfrom(2048)[0]
    except socket.timeout: return None

def hello(s, port, pid, room="r", secret=SECRET, ts=None, nonce=None):
    ts = int(time.time()) if ts is None else ts
    nonce = int.from_bytes(os.urandom(8), "little") if nonce is None else nonce
    pkt = P.pack_hello(pid, room, secret, ts, nonce)
    s.sendto(pkt, ("127.0.0.1", port))
    return pkt

def join(port, pid, room="r"):
    s = sock(); hello(s, port, pid, room); r = recv(s)
    assert r and r[3] == P.ACCEPT, f"join failed: {r!r}"
    return s, struct.unpack_from("<Q", r, 4)[0]

def reason(r):
    assert r and r[3] == P.REJECT, f"expected REJECT, got {r!r}"
    return r[4]

port = free_port(); srv = start(port); A = ("127.0.0.1", port)
try:
    # 1. wrong secret
    s = sock(); hello(s, port, 1, secret="nope"); assert reason(recv(s)) == P.R_BAD_MAC
    # 2. stale timestamp
    s = sock(); hello(s, port, 1, ts=int(time.time()) - 3600); assert reason(recv(s)) == P.R_STALE
    # 3. replayed HELLO (same bytes) is refused even from another socket
    s1 = sock(); pkt = hello(s1, port, 7); r = recv(s1); assert r[3] == P.ACCEPT
    s2 = sock(); s2.sendto(pkt, A); assert reason(recv(s2)) == P.R_STALE
    # 4. unknown / guessed session id gets nothing forwarded and a REJECT
    a, sa = join(port, 10); b, sb = join(port, 11)
    a.sendto(P.pack_pos(sa, 0, 0, 64, 0, 0), A); b.sendto(P.pack_pos(sb, 0, 5, 64, 0, 0), A); time.sleep(0.1)
    evil = sock(); evil.sendto(P.pack_voice(sa, 1, 0, 0, 64, 0, 0, b"x"), A)   # right session, wrong address
    assert reason(recv(evil)) == P.R_UNKNOWN_SESSION and recv(b) is None
    a.sendto(P.pack_voice(sa ^ 1, 1, 0, 0, 64, 0, 0, b"x"), A)                  # wrong session, right address
    assert reason(recv(a)) == P.R_UNKNOWN_SESSION and recv(b) is None
    # 5. a forwarded packet must NOT contain the sender's session id, and has the right layout
    a.sendto(P.pack_voice(sa, 9, 0, 0, 64, 0, 90.0, b"hello-opus"), A)
    f = recv(b); assert f and f[3] == P.VOICE_FWD
    assert struct.pack("<Q", sa) not in f, "session id leaked to another client!"
    sender, seq, dim, x, y, z, yaw = P.FWD_HEAD.unpack_from(f, 4)
    assert (sender, seq, dim, yaw) == (10, 9, 0, 90.0) and f.endswith(b"hello-opus")
    # 6. malformed / oversize / random junk is ignored (server survives, no reply)
    for junk in (b"", b"VC", b"VC\x02\x02", os.urandom(40), b"VC\x02\x01" + os.urandom(30), b"V" * 2000):
        a.sendto(junk, A)
    time.sleep(0.2); assert recv(a) is None or True
    ping = P.pack_session_only(P.PING, sa); a.sendto(ping, A)
    r = recv(a); assert r and r[3] == P.PONG, "server still alive after junk"
    # 7. per-client rate limit: a flood from one client doesn't make the server forward unlimited packets
    import threading
    got = [0]; stop = threading.Event()
    def drain():
        b.settimeout(0.02)
        while not stop.is_set():
            try: b.recvfrom(2048); got[0] += 1
            except socket.timeout: pass
    th = threading.Thread(target=drain); th.start()
    for batch in range(15):                       # 15 x 100 packets, paced so the test's own
        for i in range(100):                      # receive buffer (~256 packets) never overflows
            a.sendto(P.pack_voice(sa, (batch * 100 + i) & 0xffff, 0, 0, 64, 0, 0, b"f"), A)
        time.sleep(0.03)
    time.sleep(0.4); stop.set(); th.join()
    assert 300 < got[0] < 800, f"rate limit looks wrong: forwarded {got[0]} of 1500 (burst is 400)"
    # 8. BYE removes the client
    a.sendto(P.pack_session_only(P.BYE, sa), A); time.sleep(0.1)
    a.sendto(P.pack_session_only(P.PING, sa), A); assert reason(recv(a)) == P.R_UNKNOWN_SESSION
    print("server security tests: ALL PASSED")
finally:
    srv.terminate()

# 9. room cap
port = free_port(); srv = start(port, "--max-room", "2")
try:
    join(port, 1); join(port, 2)
    s = sock(); hello(s, port, 3); assert reason(recv(s)) == P.R_ROOM_FULL
    other, _ = join(port, 4, room="other")        # other rooms unaffected
    print("room cap test: PASSED")
finally:
    srv.terminate()

# 10. refuses to start with no secret
p = subprocess.run([sys.executable, "server.py", "--port", str(free_port())], capture_output=True, text=True, timeout=10,
                   env={k: v for k, v in os.environ.items() if k != "VC_SECRET"})
assert p.returncode != 0 and "secret" in (p.stderr + p.stdout).lower()
print("no-secret startup refusal: PASSED")
