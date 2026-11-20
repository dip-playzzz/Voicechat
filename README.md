# Proximity Voice Chat (LeviLauncher Android mod)

Built on the LeviLauncher mod template (preloader-android 0.2.2).

## Status
- [x] **Self Hearing** (default ON) – hear your own voice; Resound Compatible preference via `throughResound`
- [x] Mic -> Opus -> speaker loopback (`src/voice/Loopback.*`), driven by `selfHearing` (legacy: `loopbackTest`)
- [ ] True FMOD / world-mixer path so Resound reverb lands on self-hearing (falls back to direct loopback for now)
- [ ] UDP client talking to the relay server (`voicechat/server.py`, protocol in `protocol.py` / `packet.h`)
- [ ] Jitter buffer
- [x] Player position / yaw / dimension via BedrockTools' event API (`src/voice/PlayerTracker.*`)
- [x] Distance falloff + stereo pan math (`src/voice/Proximity.h`, host-tested: `tests/proximity_test.cpp`)

### Self Hearing / Resound Compatible (config)
| Key | Default | Meaning |
|-----|---------|---------|
| `selfHearing` | `true` | Hear your own voice |
| `throughResound` | `true` | Prefer routing through the game mixer (Resound FX) |
| `selfHearingVolume` | `1.0` | 0.0–2.0 linear gain (1.0 = 100%) |

## Build
Push to GitHub and let the Actions workflow build the `.levipack` (artifact: `voicechat-levipack`),
or locally with Android SDK + NDK 28.2.13676358, CMake, Ninja and PowerShell:

    ./scripts/package.ps1 -Abi arm64-v8a

Import the resulting `.levipack` in LeviLauncher's mod manager and enable it.

## Testing the loopback
1. Grant Minecraft the microphone permission in Android settings.
2. Wear headphones, launch the game, speak.
3. Check logcat for the `VoiceChat` tag and the mod logger lines
   ("Loopback running" or "Loopback failed to start").

## Requires BedrockTools
Position comes from BedrockTools' public event API (MIT; SDK headers vendored in `third_party/bedrocktools`).
Import and enable **BedrockTools** (tested against 1.5.10, built for Minecraft `1.26.60.X`) alongside this mod.
If it isn't loaded, this mod logs a warning and keeps retrying. If BedrockTools' offsets don't match your
Minecraft version, the position values will be wrong or crash, so use the matching BedrockTools release.

Expected log line every 5 s once in a world:
`player pos=(x, y, z) yaw=... dim=0`

## Host-side tests (run on a PC, no Android needed)
    g++ -std=c++20 -Isrc tests/proximity_test.cpp -o proximity_test && ./proximity_test
    g++ -std=c++20 -DVOICECHAT_TEST -g -O1 -fsanitize=address,undefined -Isrc \
        -Ithird_party/bedrocktools/include tests/tracker_lifetime_test.cpp \
        src/voice/PlayerTracker.cpp -pthread -o lifetime_test && ./lifetime_test 3000

`tracker_lifetime_test` publishes player ticks on one thread while another repeatedly attaches,
stops and destroys the tracker. The previous implementation fails it with a heap-use-after-free.

## Microphone permission
The mod runs inside the LeviLauncher app, so the microphone permission belongs to LeviLauncher
(`org.levimc.launcher`), not Minecraft. On startup the mod logs
`Microphone: app=... declared=... granted=...`:
- `granted=true`   -> audio starts.
- `declared=false` -> LeviLauncher's manifest has no RECORD_AUDIO, so Android can't grant it.
- `granted=false`  -> the mod asks Android to show the prompt (config `requestMicPermission`);
                      you can also allow it in Settings > Apps > LeviLauncher > Permissions.

## Launcher microphone service (`micSource = "launcher"`, default)
Newer LeviLauncher builds with voice support capture the microphone themselves
(`org.levimc.launcher.voice.VoiceCaptureService`) and stream it to the mod:
- The mod listens on the abstract unix socket `@org.levimc.voicechatlevi.audio`; the service connects.
- Data: raw PCM, 16-bit signed, mono, 16 kHz, no header (640-byte / 20 ms writes).
- The mod never writes to the socket (a reply starting with "VCLS" would make the service stand down).
- Only connections from our own app UID are accepted.
Expected log lines: `listening for the launcher microphone stream`, `launcher connected`, then every 5 s
`mic stream: 250 frames in 5 s, peak -23 dBFS`.
Host test: `g++ -std=c++20 -g -O1 -fsanitize=address,undefined -Isrc tests/mic_socket_test.cpp src/voice/MicSocketSource.cpp -pthread -o ms && ./ms`

## Crash fix (Minecraft 1.26.52)
A crash dump from a 1.26.52 device showed a SIGSEGV inside the position-reading code: the vendored
BedrockTools SDK reads Player memory at offsets made for 1.26.60.*, and on another build the loaded
pointer was garbage. Now (1) position tracking only starts on 1.26.60.* (version taken from the mod's
folder name; override with `allowUnsupportedVersion`), and (2) every game-memory read goes through
`safeRead()` (kernel-checked copy) and a plausibility filter, so a bad offset skips the sample instead of
crashing. Tests: `tests/safety_test.cpp`.

## Internet relay (different networks)
See `relay/README.md`. `useRelay=true` makes the mod join the relay and send the player's position;
it logs connection status. Audio is not yet routed through it (next step).
Tests: `relay/test_server.py`, `tests/relay_integration.py` (C++ client against the real server).


## Voice chat (simple proximity voice over the internet relay)
Targets **BedrockTools 1.5.12 / Minecraft 1.26.6X.X** (1.26.60 - 1.26.69; other versions skip position tracking
so they can't crash the game).

Data flow:
    LeviLauncher mic service -> gate + Opus (20 kbps) -> relay (with your position) -> nearby players
    relay -> jitter buffer -> Opus decode (lost frames concealed) -> distance volume + left/right pan -> speaker

Settings (mod config): `useRelay=true`, `serverHost`, `serverPort`, `room`, `secret`, `micSource="launcher"`,
`radius` (blocks), `micGainPercent`, `micGate`. See `relay/README.md` to run the server.
Everyone in a room needs the same `secret`; the server decides who is within `radius`.

Status lines in the log (every 10 s): `Relay: connected | voice packets heard ... | mic connected, voice frames
sent N (speaking), speakers heard now M`.

Needs: a LeviLauncher build with the microphone service (it declares RECORD_AUDIO), BedrockTools 1.5.12 enabled
(for the position), headphones recommended. Not encrypted; not yet tested on a phone.

Run every PC-side test: `sh tests/run_host_tests.sh` (needs g++, python3, libopus.so.0).

## Java Simple Voice Chat players (bridge)
`bridge/` is a Paper plugin that connects Simple Voice Chat players to the same relay, so Java SVC players and Bedrock
mod players can hear each other by proximity. See `bridge/README.md`. Built by its own workflow (`bridge.yml`).

## Mod Menu card
On start the mod registers a **Proximity Voice Chat** card in LeviLauncher's Mod Menu: an on/off switch, "Use internet
relay", microphone gain, voice gate and voice radius. Changes are saved to config.json; gain, gate and radius apply
immediately, the on/off switch and the relay switch restart the voice services (on a background thread).
Room and secret are text, so they stay in config.json. Log line: `Mod Menu: 'Proximity Voice Chat' card registered`,
or `No Mod Menu card (<reason>)` if the launcher has none. Tested on a PC against stand-ins for an older and a newer
launcher layout (`tests/modmenu_test.cpp`); not yet seen on a phone.
