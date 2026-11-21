# VoiceChatBridge (Paper plugin)

Lets players who use the **Simple Voice Chat** (SVC) mod talk, by proximity, with **Bedrock players using the
Proximity Voice Chat mod**, through the same relay.

    Java player (SVC mod) --mic--> plugin --relay--> Bedrock mod players nearby hear them
    Bedrock mod player  --relay--> plugin --SVC--> Java players nearby hear them (positioned, with direction)

## Install
1. Server: Paper (or a fork) 1.21+, **Simple Voice Chat 2.5+** installed and working. Geyser/Floodgate optional.
2. Build the jar: push this repo; GitHub Actions -> "Bridge plugin" -> artifact `voicechat-bridge-jar`.
3. Put the jar in `plugins/`, start the server once, stop it.
4. Edit `plugins/VoiceChatBridge/config.yml`:
   `relay.host/port` (your relay), `relay.room` (same as the Bedrock players), `relay.secret`, `radius` (same as the relay's `--radius`).
5. Start the relay with room capacity for everyone: `python3 relay/server.py --secret ... --max-room 100`.
   If Simple Voice Chat already uses UDP 24454 on the same host, run the relay on a different UDP port
   (e.g. `--port 24455`) and set that port in the bridge and every Bedrock mod config.
6. Start the server. Log: `Voice bridge ready: ...`, then `Relay: connected ...` for each Java player who joins voice chat.

## What it does and does not do
- Opus packets are passed through unchanged (no re-encoding); positions come from the server for Java players and from the
  mod (BedrockTools) for Bedrock players. Same dimension and distance rules as the relay.
- **Not forwarded to the relay:** whispers, players inside a Simple Voice Chat *group*, muted/disabled players.
- Java players hear each other through SVC as usual; the plugin ignores relay voice that came from other Java players (no echo).
- Everything a Java player says in proximity is sent to the relay server you configured. Only point it at a relay you trust.
- Voice on the relay link is not encrypted (see relay/README.md). Not Folia compatible.

## Status: what was and was not tested
- Tested on a PC: the Java relay client against the real relay (login, delivery, distance, dimension, reconnect) and the id/sequence logic.
- Compiles against the real Simple Voice Chat API sources. The Paper/Bukkit part was only compile-checked against stand-in classes.
- **Not tested on a real server with real Simple Voice Chat clients.** Expect to fix small things on first run.
- If Gradle can't resolve a dependency, edit `bridge/gradle.properties` (`paperVersion`, `svcApiVersion`).

Tests: `sh bridge/test/run_java_tests.sh` (JDK 21 and Python required). ID/sequence checks use a test-only SVC type stub; the relay test uses the real Python relay. A full plugin compile is attempted only when SVC API sources and Paper stand-ins are provided, and neither test substitutes for a real server.
