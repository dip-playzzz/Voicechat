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
If Simple Voice Chat is on the same host and already uses UDP 24454, choose another relay port
(e.g. `--port 24455`), open it, and set that port in both the mod and bridge configs.

## Client config (each player, mod settings)
    useRelay    = true
    serverHost  = your server's IP or domain
    serverPort  = 24454
    room        = any name; players in the same room hear each other
    secret      = the room secret (never sent over the network)
    playerId    = 0 (random) or a unique number

The default `useRelay` is false, the default host `127.0.0.1` means the phone itself, and the
secret starts empty. Set all three before expecting multiplayer to work. Every player also needs
the mod, a working microphone and current BedrockTools positions in a supported world. Use a
separate room per Minecraft world: room membership is configured manually, not verified by the relay.

On start the mod logs `Relay: connected ...` and every 10 s reports connection status, packets sent,
mic level and speakers heard. If it says `Relay rejected us: wrong room secret` / `clock off by N s`,
fix that setting. The server forwards only to clients with a recent position (within 3 seconds),
in the same room and dimension and within its radius. The client also applies its own radius;
configure them to match. Server capacity defaults to 32 players per room, and a client can mix
up to 16 simultaneous speakers.

## What it protects, and what it doesn't
- Needs the room secret to join (HMAC proof, replay-protected, clock must be within 60 s).
- Random session ids; forwarded packets never contain the sender's session.
- Per-client rate limit, room and server caps, malformed packets dropped silently.
- **Voice and positions are NOT encrypted** and anyone on the path can listen. Adding per-room
  encryption is the next hardening step; until then don't treat it as private.

## Tests
    python3 test_server.py                    # relay security tests (in this folder)
    python3 test_multiplayer.py                # rooms, dimensions, positions and reconnects
    python3 ../tests/relay_integration.py     # C++ client vs real server, incl. server restart
    python3 ../tests/voice_e2e.py             # three clients, real Opus audio and UDP
