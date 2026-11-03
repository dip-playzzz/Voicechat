# Proximity Voice Chat (LeviLauncher Android mod)

Built on the LeviLauncher mod template (preloader-android 0.2.2).

## Status
- [x] Mic -> Opus -> speaker loopback (`src/voice/Loopback.*`), toggled by `loopbackTest` in config
- [ ] UDP client talking to the relay server (`voicechat/server.py`, protocol in `protocol.py` / `packet.h`)
- [ ] Jitter buffer
- [x] Player position / yaw / dimension via BedrockTools' event API (`src/voice/PlayerTracker.*`)
- [x] Distance falloff + stereo pan math (`src/voice/Proximity.h`, host-tested: `tests/proximity_test.cpp`)

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
