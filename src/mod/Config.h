#pragma once

#include <string>
#include <string_view>

#include <pl/Config.hpp>

namespace voicechat {

struct ModConfig {
    int version = 1;
    bool enabled = true;
    // Mic -> Opus -> speaker test. Turn off once networking is wired up.
    bool loopbackTest = true;
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
        if (name == "loopbackTest")
            return {.title = "Loopback test",
                    .description = "Play your own microphone back through the speaker (use headphones)."};
        if (name == "serverHost")
            return {.title = "Server host", .description = "Voice relay server address."};
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
                    .description = "Developer only. Position tracking is made for 1.26.60.*; turning this on elsewhere can crash the game."};
        if (name == "requestMicPermission")
            return {.title = "Request microphone permission",
                    .description = "Show Android's microphone prompt when the permission is missing."};
        if (name == "logPosition")
            return {.title = "Log position",
                    .description = "Write the player position to the log every 5 seconds (debug)."};
        return {};
    }
};
