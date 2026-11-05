#pragma once
// The game version is in the mod's own folder name, e.g.
//   .../org.levimc.launcher/minecraft/Minecraft_1.26.60.4/mods/Proximity Voice Chat
#include <string>

namespace voicechat {

struct GameVersion {
    bool valid = false;
    int major = 0, minor = 0, patch = 0, build = 0;
    std::string text;
};

inline GameVersion parseGameVersion(const std::string &path) {
    GameVersion v;
    const std::string key = "Minecraft_";
    const auto at = path.find(key);
    if (at == std::string::npos) return v;
    std::size_t i = at + key.size();
    int parts[4] = {0, 0, 0, 0};
    int count = 0;
    const std::size_t begin = i;
    while (count < 4 && i < path.size() && path[i] >= '0' && path[i] <= '9') {
        int n = 0;
        while (i < path.size() && path[i] >= '0' && path[i] <= '9') n = n * 10 + (path[i++] - '0');
        parts[count++] = n;
        if (i < path.size() && path[i] == '.' && count < 4) i++; else break;
    }
    if (count < 3) return v;
    v.valid = true;
    v.major = parts[0]; v.minor = parts[1]; v.patch = parts[2]; v.build = parts[3];
    v.text = path.substr(begin, i - begin);
    return v;
}

// The memory offsets in the vendored BedrockTools SDK are for 1.26.60.* only.
inline bool isSupportedGameVersion(const GameVersion &v) {
    return v.valid && v.major == 1 && v.minor == 26 && v.patch == 60;
}

} // namespace voicechat
