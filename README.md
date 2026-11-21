# Proximity Voice Chat for LeviLauncher

An Android LeviLauncher mod for **proximity voice chat** in Minecraft Bedrock. Each player runs the mod; a separate UDP relay connects them and forwards Opus audio to other players in the same room, dimension and radius. A Java Simple Voice Chat bridge is available separately in [`bridge/`](bridge/README.md).

## Why it wasn't working

The previous revision had a damaged section in `src/mod/VoiceMod.cpp`, so the Android mod could not compile and the relay/voice session was never created. Multiplayer also needs configuration: **`useRelay` defaults to `false`**, the default host `127.0.0.1` points back to the phone itself, and the default room secret is empty. Hearing yourself is only a microphone test; it does not connect you to other players.

This revision connects the multiplayer voice pipeline, supports both launcher and direct microphones, and tests the actual mod startup with simulated Android audio. **It has not yet been tested on real phones**, so an on-device test is still needed.

## What every player needs

- LeviLauncher and this mod installed/enabled on each Android device.
- **BedrockTools 1.5.12**, enabled alongside the mod, and a matching Minecraft version. The vendored offsets target **Minecraft 1.26.60–1.26.69**; other versions disable position tracking by default to avoid crashes. Without a current position, voice is not sent or played.
- A launcher that declares Android's `RECORD_AUDIO` permission, with microphone permission granted to **LeviLauncher**, not the separate Minecraft app.
- Everyone must join the same Minecraft world/server and configure the same reachable voice relay, port, room and secret. The relay room is **not automatically linked** to a Minecraft server: use a distinct room for each world.
- Headphones are recommended to avoid microphone feedback, especially if self-hearing is enabled.

## Multiplayer setup

### 1. Start a relay

Run one relay on a computer/VPS reachable by every phone. Python 3.8+ is required; there are no Python package dependencies.

```sh
# Generate a long random secret and share it with your players privately.
python3 -c 'import secrets; print(secrets.token_urlsafe(32))'
# Set VC_SECRET to that generated value in your server environment, then:
python3 relay/server.py --port 24454 --radius 32 --max-room 32
```

Open/forward **UDP port 24454** on the relay's firewall/router. For internet play use a public server IP/domain or a UDP tunnel; a home server behind CGNAT may not be reachable. For LAN play use the host computer's LAN IP, not `127.0.0.1`. Only the relay needs a reachable UDP port; players make outgoing connections. See [`relay/README.md`](relay/README.md) for more options.

### 2. Configure each player's mod

Edit the mod's `config/config.json` on each device (the file is generated in the mod directory). Set or change these fields, replacing the example values:

```json
{
  "enabled": true,
  "useRelay": true,
  "serverHost": "YOUR_RELAY_IP_OR_DOMAIN",
  "serverPort": 24454,
  "room": "my-world",
  "secret": "THE_SAME_GENERATED_SECRET",
  "playerId": 0,
  "radius": 32,
  "micSource": "launcher",
  "selfHearing": false
}
```

Keep the other generated settings. `playerId: 0` chooses a random ID; if assigning IDs manually, every player in a room must use a different positive ID. Use the same `radius` as the relay: the relay limits delivery, and each client also applies its own distance falloff. The relay defaults to 32 players per room and the client mixes up to 16 simultaneous speakers.

`micSource` can be:

- **`launcher`** (default): receive 16 kHz mono audio from a LeviLauncher build with its `VoiceCaptureService`. The mod listens on `@org.levimc.voicechatlevi.audio` and accepts only the same app UID. A launcher without this service will wait indefinitely for it.
- **`direct`**: open the microphone with Oboe after LeviLauncher has microphone permission. This can be useful when the launcher does not provide the service. If the launcher is already using the mic, stop its capture service or use `launcher` instead.

The `VoiceCaptureService` in [LeviLaunchroid-Voice1](https://github.com/qqchoke99-bot/LeviLaunchroid-Voice1/blob/c98f20cecbac216f305ce93d4ae8a4a29f4e586c/app/src/main/java/org/levimc/launcher/voice/VoiceCaptureService.kt) already uses Android's voice-communication input and attempts to attach `AcousticEchoCanceler`. This mod does not send its optional control greeting; that launcher times out after up to 1.5 seconds and falls back to requesting AEC with the noise suppressor off. Its `VoiceChatLevi` log reports whether the effect attached. AEC availability and effectiveness depend on the device; an attached effect does not guarantee that speaker/game audio is removed.

Restart the mod/game after editing text fields. The Mod Menu card can also toggle the mod/relay and adjust mic gain, gate and radius. All players need to be in a supported world for positions to update; then speak near one another.

## Self-hearing and settings

**Self-hearing is off by default:** `selfHearing: false` prevents local microphone playback, but still permits sending your microphone to other players. Speaker audio can still be picked up by the microphone. To avoid an acoustic feedback loop, use headphones and start with the device volume low if you deliberately enable monitoring. Even a low monitoring gain cannot guarantee safe volume on every device. Stop playback immediately if you hear a squeal.

If enabled during multiplayer, self-hearing uses the same mic stream rather than opening a second microphone. `selfHearingVolume` is a linear 0.0–2.0 gain with a new default of 0.1; standalone direct self-hearing now defaults to 0 dB mic gain instead of +6 dB. For safety, the first load of a pre-v4 config disables self-hearing, sets `loopbackTest` to false and resets these two monitoring gains; other settings, including relay configuration, are preserved. You may explicitly opt in again afterwards. `throughResound` is a preference only: the game-mixer/Resound effects path is not implemented, so self-hearing uses direct playback.

For multiplayer, adjust `micGainPercent` (100 = unchanged) and `micGate` (0 = always send). A gate cannot reliably filter loud game sounds from speech. `micInputPreset` selects the **direct** Android input mode, not the launcher's capture service. `micGainDb` applies only to the standalone direct self-hearing test and is clamped to -60 to +12 dB. Android's `voice_communication` preset may enable echo cancellation on some devices, but may also suppress speech and cannot be relied on to eliminate feedback.

## Experimental WebRTC audio processing

The multiplayer microphone path can optionally use [WebRTC Audio Processing](https://github.com/thesofproject/webrtc-audio-processing) at the existing 16 kHz rate. Both options default to **off**, including when an existing config is upgraded. For example, to opt into moderate noise suppression, change these fields in the existing `config.json` while the game is closed (keep your other settings):

```json
{
  "webrtcNoiseSuppression": true,
  "webrtcVoiceAec": false
}
```

- `webrtcNoiseSuppression` uses WebRTC's built-in suppressor on launcher or direct **multiplayer** microphone audio. It may also suppress speech; if speech is cut off, disable it or lower the separate `micGate`. It does not remove echo reliably. It is not used by the standalone self-hearing test.
- `webrtcVoiceAec` is an **experimental, opt-in, voice-only** echo canceller for the direct microphone. Its reference is the stereo audio produced by this mod's separate Oboe voice stream. **Minecraft's game sound is not in that reference**, so this cannot cancel all game-sound echo. Device volume, routing and actual hardware latencies are not measured. `webrtcAecDelayMs` is a rough starting estimate (default 50, clamped to 0–500 ms), not a calibrated delay.
- To avoid deliberately stacking echo cancellers, software AEC is skipped for `micSource: "launcher"` (the inspected launcher already requests Android AEC), the direct `voice_communication` preset, or enabled self-hearing. Some OEMs may still apply effects with other presets. The reference is copied on the output callback and processed on a worker; if the bounded reference or direct-capture queue overruns, WebRTC processing is bypassed until restart and the unprocessed mic is sent. Check the mod's status/log for the actual mode.

**Neither option is feedback protection.** Keep self-hearing off unless deliberately needed; use headphones and stop if a squeal occurs. No Android hardware or full-game-audio AEC behavior has been validated yet.

The suggested [noise-cancellation-cpp](https://github.com/jeffasante/noise-cancellation-cpp) is **not bundled**: its example depends on LibTorch and a model, assumes a 48 kHz offline pipeline and a large context window, and has not been validated as a low-latency Android streaming processor. Its current frame path also does not return the computed hybrid fusion output. WebRTC's built-in suppressor is the available alternative here, not an integration of that project.

## Troubleshooting

Check logcat for the `VoiceChat` tag and the mod logger:

- **No `Relay: connected`**: check `useRelay`, `serverHost`, UDP port/firewall and that the relay is running. A phone's `127.0.0.1` is not another player's computer. A DNS name with multiple addresses is retried if one is unreachable.
- **Wrong room secret / clock error**: all players need the exact same secret and room; set their device clocks correctly.
- **Waiting for LeviLauncher's voice service**: use a launcher with the service, or try `micSource: "direct"`. For direct mode grant microphone permission to LeviLauncher in Android settings; some launchers do not declare it at all.
- **No player position**: join a world, enable the matching BedrockTools release and use a supported Minecraft version. A stale position is dropped after ticks stop; the relay also stops delivering to stale listeners.
- **Connected but silent**: check mic level and relay packet counts in the 10-second status line, lower `micGate`, and check both players' room, dimension and distance. The relay radius and each client's radius both limit audio.
- **Only hear yourself**: self-hearing is a local test. Enable/configure the relay on every player.

## Build

The GitHub Actions **Build** workflow produces the `voicechat-levipack` artifact with separate `arm64-v8a` and `armeabi-v7a` packages. Import the one matching the device in LeviLauncher's mod manager and enable it. The modern NDK can require NEON even in the 32-bit build; non-NEON devices are not supported. Locally, Android SDK + NDK 28.2.13676358, CMake, Ninja, Python, Meson and PowerShell are needed:

```powershell
python -m pip install meson==1.5.2
./scripts/package.ps1 -Abi arm64-v8a
```

The build downloads pinned WebRTC APM and Abseil sources and links them statically; their notices are included in the package. `-DVOICECHAT_ENABLE_WEBRTC=OFF` disables that dependency when configuring CMake (requested processing then stays off and is logged). The workflow runs on a push to `main`, on a pull request, or manually from Actions. A failed compiler/package command fails the workflow instead of silently continuing.

## Host tests

On Linux, install `g++`, Python 3, CMake, Ninja, Meson 1.5.2 and Opus (e.g. `libopus-dev`). The following also compiles the real mod lifecycle against the real preloader headers and runs a simulated-device test of both microphone paths against the real relay:

```sh
cmake -S . -B build-host -DVOICECHAT_BUILD_HOST_INTEGRATION=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-host --target voicechat_mod_compile_check voicechat_mod_integration_test voicechat_webrtc_test -j 2
build-host/voicechat_webrtc_test
PRELOADER_INC="$PWD/build-host/_deps/preloader_android-src/include" sh tests/run_host_tests.sh
python3 tests/voice_mod_integration.py build-host/voicechat_mod_integration_test
```

The WebRTC test uses synthetic signals with a known, matched echo reference; it is not proof of real-phone AEC or speech quality. Other tests include simultaneous speakers, room/dimension/distance isolation, stale positions, relay reconnects, and the launcher/direct input paths. Android permission, Oboe hardware, actual Bedrock memory offsets and the Java bridge still require real-device/server testing.

## Privacy

The room secret authenticates clients, but **voice and positions are not encrypted**. Anyone on the network path or running the relay may be able to hear them. The relay trusts client-reported positions and does not verify Minecraft identity or world membership. Use a relay and room members you trust; do not treat this as private communication.
