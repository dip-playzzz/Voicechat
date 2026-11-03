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
        if (name == "logPosition")
            return {.title = "Log position",
                    .description = "Write the player position to the log every 5 seconds (debug)."};
        return {};
    }
};
