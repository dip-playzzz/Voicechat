#include "mod/VoiceMod.h"

#include <filesystem>

#include <pl/Mod.hpp>

namespace voicechat {

VoiceMod &VoiceMod::instance() {
    static VoiceMod instance;
    return instance;
}

VoiceMod::VoiceMod() : mSelf(*ll::mod::NativeMod::current()) {}

bool VoiceMod::load() {
    auto &self = getSelf();
    self.getLogger().debug("Loading...");

    std::error_code ec;
    std::filesystem::create_directories(self.getDataDir(), ec);
    if (ec) {
        self.getLogger().error("Failed to create data directory {}: {}", self.getDataDir().string(),
                               ec.message());
        return false;
    }

    std::filesystem::create_directories(self.getConfigDir(), ec);
    if (ec) {
        self.getLogger().error("Failed to create config directory {}: {}",
                               self.getConfigDir().string(), ec.message());
        return false;
    }

    mConfigFile.emplace();
    if (!mConfigFile->load()) {
        self.getLogger().warn("Failed to load typed config");
        return false;
    }
    mConfig = mConfigFile->value();

    self.getLogger().info("Loaded {} from {}", self.getName(), self.getModDir().string());
    return true;
}

bool VoiceMod::enable() {
    auto &self = getSelf();
    self.getLogger().debug("Enabling...");
    if (!mConfig.enabled) {
        self.getLogger().info("voice chat is disabled by config");
        return true;
    }

    // Position tracking (needs the BedrockTools mod enabled alongside this one).
    mTracker = std::make_unique<PlayerTracker>(
        [this](const std::string &msg) { getSelf().getLogger().info("{}", msg); }, mConfig.logPosition);
    mTracker->start();

    if (mConfig.loopbackTest) {
        mLoopback = std::make_unique<Loopback>();
        if (!mLoopback->start()) {
            // Don't fail the mod (and risk blocking the game) just because audio didn't open.
            // Most likely cause: Minecraft doesn't hold the microphone permission yet.
            self.getLogger().error("Loopback failed to start - grant the Microphone permission to the LeviLauncher app (Android Settings > Apps)");
            mLoopback.reset();
        } else {
            self.getLogger().info("Loopback running - speak, you should hear yourself");
        }
    } else {
        self.getLogger().info("Networked voice not implemented yet (server {}:{}, radius {})",
                              mConfig.serverHost, mConfig.serverPort, mConfig.radius);
    }
    return true;
}

bool VoiceMod::disable() {
    getSelf().getLogger().debug("Disabling...");
    if (mTracker) {
        mTracker->stop();
        mTracker.reset();
    }
    if (mLoopback) {
        mLoopback->stop();
        mLoopback.reset();
    }
    return true;
}

bool VoiceMod::unload() {
    getSelf().getLogger().debug("Unloading...");
    mConfigFile.reset();
    return true;
}

} // namespace voicechat
