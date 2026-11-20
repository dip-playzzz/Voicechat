# Voice relay (internet play)

Players on **different networks** connect to one relay server over UDP. Players only make outgoing
connections, so no router setup is needed on their side. Only the server needs a reachable address.

## Run the server
    python3 server.py --port 24454 --radius 32 --secret YOUR_LONG_RANDOM_SECRET [--max-room 100]
or one secret per room:
    python3 server.py --secrets-file rooms.json        # {"survival": "s1", "creative": "s2"}

Needs Python 3.8+, nothing else. It refuses to start without a secret.

## Make it reachable (pick one)
1. **A small VPS** (any provider with a public IPv4). Open **UDP 24454** in its firewall, e.g.
   `sudo ufw allow 24454/udp`, then run the server there. Most reliable.
2. **A home PC + router port-forward** of UDP 24454. Fails if your ISP uses CGNAT (the router's WAN
   address isn't a public one). Check before spending time on it.
3. **A UDP tunnel service** (e.g. playit.gg offers UDP tunnels). I haven't checked their current terms.
A phone running Termux can run the server but is almost never reachable from outside.

## Client config (each player, mod settings)
    useRelay    = true
    serverHost  = your server's IP or domain
    serverPort  = 24454
    room        = any name; players in the same room hear each other
    secret      = the room secret (never sent over the network)
    playerId    = 0 (random) or a unique number

On start the mod logs `Relay: connected ...` and every 10 s `Relay: connected | voice packets heard ...`.
If it says `Relay rejected us: wrong room secret` / `clock off by N s`, fix that setting.

## What it protects, and what it doesn't
- Needs the room secret to join (HMAC proof, replay-protected, clock must be within 60 s).
- Random session ids; forwarded packets never contain the sender's session.
- Per-client rate limit, room and server caps, malformed packets dropped silently.
- **Voice and positions are NOT encrypted** and anyone on the path can listen. Adding per-room
  encryption is the next hardening step; until then don't treat it as private.

## Tests
    python3 test_server.py                    # relay security tests (in this folder)
    python3 ../tests/relay_integration.py     # C++ client vs real server, incl. server restart
