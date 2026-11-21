#pragma once

#include <string>
#include <string_view>

#include <pl/Config.hpp>

#include "voice/AudioDefaults.h"

namespace voicechat {

inline constexpr int kConfigVersion = 4;

struct ModConfig {
    int version = kConfigVersion;
    bool enabled = true;

    // Self-hearing is opt-in: an open speaker and mic can form a loud feedback loop.
    // The game-mixer/Resound path is not implemented; monitoring uses direct Oboe playback.
    bool selfHearing = audio_defaults::selfHearing;
    bool throughResound = true;   // Resound-compatible routing preference (not implemented)
    float selfHearingVolume = audio_defaults::selfHearingVolume; // 0.0 .. 2.0

    // VoiceCommunication may request Android AEC/NS, but can suppress speech on some phones.
    // Options: voice_recognition | voice_communication | generic | unprocessed | camcorder
    std::string micInputPreset = "voice_recognition";
    float micGainDb = audio_defaults::selfHearingMicGainDb; // standalone direct self-hearing only

    // Retained so old config files still load. selfHearing is the authoritative switch.
    bool loopbackTest = false;

    std::string serverHost = "127.0.0.1";
    int serverPort = 24454;
    int radius = 32; // blocks
    // Log the local player's position every 5 s (proves the BedrockTools hook works).
    bool logPosition = true;
    // Where microphone audio comes from:
    //  "launcher" = the LeviLauncher VoiceCaptureService streams it to us (needs a launcher build with voice support)
    //  "direct"   = this mod opens the mic itself (needs the launcher to declare RECORD_AUDIO)
    std::string micSource = "launcher";
    // Ask Android to show the microphone prompt if the permission isn't granted yet.
    bool requestMicPermission = true;
    // Developer switch: read player memory even on a Minecraft build the offsets weren't made for.
    // Leave OFF; a mismatch can still crash the game.
    bool allowUnsupportedVersion = false;
    // Internet voice relay (relay/server.py). Uses serverHost / serverPort above.
    bool useRelay = false;
    std::string room = "default"; // players in the same room can hear each other
    std::string secret = "";      // room secret (must match the server's); never sent over the network
    int playerId = 0;             // 0 = pick a random id each launch
    int micGainPercent = 100;     // 100 = unchanged, 200 = twice as loud
    int micGate = 350;            // quieter than this = not sent (background noise); 0 = always send
    // Experimental WebRTC processing, multiplayer only. Voice AEC uses only
    // this mod's playback, not Minecraft's game mix; neither is feedback safety.
    bool webrtcVoiceAec = false;
    bool webrtcNoiseSuppression = false;
    int webrtcAecDelayMs = 50; // unmeasured stream-delay estimate, clamped to 0..500 ms
};

nlohmann::json makeDefaultConfigJson();
nlohmann::json makeConfigSchemaJson();

} // namespace voicechat

template <> struct pl::config::Schema<voicechat::ModConfig> {
    static constexpr std::string_view title = "Voice Chat Config";
    static constexpr std::string_view description = {};

    static constexpr FieldSchema field(std::string_view name) {
        if (name == "version")
            return {.title = "Version", .readOnly = true};
        if (name == "enabled")
            return {.title = "Enabled", .description = "Turns the voice chat mod on or off."};
        if (name == "selfHearing")
            return {.title = "Self Hearing",
                    .description = "Monitor your microphone (default OFF). Can cause loud feedback. "
                                   "Use headphones and start with the device volume low."};
        if (name == "throughResound")
            return {.title = "Through Resound",
                    .description = "Preference only: game-mixer/Resound routing is not implemented; "
                                   "self-hearing uses direct playback."};
        if (name == "selfHearingVolume")
            return {.title = "Self Hearing Volume",
                    .description = "Own-voice playback gain (0.0 = silent, 1.0 = 100%, 2.0 = 200%). "
                                   "Even a low value can cause feedback."};
        if (name == "micInputPreset")
            return {.title = "Mic input preset",
                    .description = "Android capture mode. Use voice_recognition if speech is missing "
                                   "and only noise remains. Options: voice_recognition, "
                                   "voice_communication, generic, unprocessed, camcorder."};
        if (name == "micGainDb")
            return {.title = "Mic gain (dB)",
                    .description = "Standalone direct self-hearing only; clamped to -60 to +12 dB. "
                                   "For multiplayer use Microphone gain (%)."};
        if (name == "loopbackTest")
            return {.title = "Loopback test (legacy)",
                    .description = "Retained for old configs. Use Self Hearing to control playback."};
        if (name == "serverHost")
            return {.title = "Server host", .description = "Voice relay address. 127.0.0.1 means this phone, not another device."};
        if (name == "serverPort")
            return {.title = "Server port", .description = "Voice relay server UDP port."};
        if (name == "radius")
            return {.title = "Radius", .description = "How far voices carry, in blocks."};
        if (name == "micSource")
            return {.title = "Microphone source",
                    .description = "\"launcher\" = audio streamed by LeviLauncher's voice service (default), "
                                   "\"direct\" = this mod opens the microphone itself."};
        if (name == "micGainPercent")
            return {.title = "Microphone gain (%)", .description = "100 = unchanged. Raise it if people say you are quiet."};
        if (name == "micGate")
            return {.title = "Voice gate", .description = "Sounds quieter than this are not sent. Lower it if your voice gets cut off; 0 = always send."};
        if (name == "webrtcVoiceAec")
            return {.title = "Experimental voice-only WebRTC AEC",
                    .description = "Off by default. Only cancels some of this mod's voice playback, not Minecraft game sound. "
                                   "Direct mic only, without voice_communication or self-hearing. Not feedback protection."};
        if (name == "webrtcNoiseSuppression")
            return {.title = "WebRTC noise suppression",
                    .description = "Off by default. Moderate suppression for multiplayer capture; may affect speech "
                                   "and does not replace echo cancellation."};
        if (name == "webrtcAecDelayMs")
            return {.title = "Experimental AEC delay estimate (ms)",
                    .description = "An estimate, not a measurement of device latency. Clamped to 0-500 ms."};
        if (name == "useRelay")
            return {.title = "Use internet relay", .description = "Connect to the voice relay server (serverHost / serverPort)."};
        if (name == "room")
            return {.title = "Room", .description = "Players in the same room can hear each other."};
        if (name == "secret")
            return {.title = "Room secret", .description = "Must match the relay server's secret for this room."};
        if (name == "playerId")
            return {.title = "Player ID", .description = "0 = random each launch. Must be unique within a room."};
        if (name == "allowUnsupportedVersion")
            return {.title = "Allow unsupported Minecraft version",
                    .description = "Developer only. Position tracking is made for 1.26.60-1.26.69; turning this on elsewhere can crash the game."};
        if (name == "requestMicPermission")
            return {.title = "Request microphone permission",
                    .description = "Show Android's microphone prompt when the permission is missing."};
        if (name == "logPosition")
            return {.title = "Log position",
                    .description = "Write the player position to the log every 5 seconds (debug)."};
        return {};
    }
};
