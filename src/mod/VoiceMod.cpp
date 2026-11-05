#include "mod/VoiceMod.h"

#include <chrono>
#include <filesystem>

#include "voice/AndroidPermission.h"
#include "voice/GameVersion.h"

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

    // Position tracking reads game memory at fixed offsets that only match Minecraft 1.26.60.*.
    // On any other build that can crash the game, so refuse to start it there.
    const GameVersion version = parseGameVersion(self.getModDir().string());
    if (isSupportedGameVersion(version) || mConfig.allowUnsupportedVersion) {
        self.getLogger().info("Minecraft {} - position tracking enabled", version.valid ? version.text : "(unknown)");
        mTracker = std::make_unique<PlayerTracker>(
            [this](const std::string &msg) { getSelf().getLogger().info("{}", msg); }, mConfig.logPosition);
        mTracker->start();
    } else {
        self.getLogger().error(
            "Minecraft {} is not supported (position offsets are for 1.26.60.*). Position tracking is OFF "
            "to avoid crashing the game. Use Minecraft 1.26.60.* with the matching BedrockTools.",
            version.valid ? version.text : "(unknown)");
    }

    if (mConfig.loopbackTest && mConfig.micSource == "launcher") {
        // The launcher's VoiceCaptureService owns the microphone and permission prompt;
        // we only listen for its audio stream.
        mSocketLoop = std::make_unique<SocketLoopback>(
            [this](const std::string &msg) { getSelf().getLogger().info("{}", msg); });
        if (!mSocketLoop->start()) {
            self.getLogger().error("Could not start the launcher-mic receiver (see lines above)");
            mSocketLoop.reset();
        } else {
            self.getLogger().info("Waiting for LeviLauncher's voice service (needs a launcher build with voice support)");
        }
    } else if (mConfig.loopbackTest) {
        {
            std::lock_guard<std::mutex> lock(mAudioMu);
            mAudioStop = false;
        }
        // Off the game thread: may have to wait for the user to answer the permission prompt.
        mAudioThread = std::thread([this] { audioStartupLoop(); });
    } else {
        self.getLogger().info("Networked voice not implemented yet (server {}:{}, radius {})",
                              mConfig.serverHost, mConfig.serverPort, mConfig.radius);
    }
    return true;
}

void VoiceMod::audioStartupLoop() {
    auto &log = getSelf().getLogger();
    auto keepWaiting = [this](std::chrono::milliseconds d) {
        std::unique_lock<std::mutex> lk(mAudioMu);
        return !mAudioCv.wait_for(lk, d, [this] { return mAudioStop; });
    };

    bool requested = false;
    for (int i = 0; i < 90; i++) { // ~3 minutes of polling, then give up
        const MicStatus st = queryMicPermission();
        if (!st.ok) {
            if (i == 0 || i % 15 == 0) log.warn("Microphone check not ready yet: {}", st.error);
        } else {
            if (i == 0)
                log.info("Microphone: app={} declared={} granted={}", st.package, st.declared, st.granted);
            if (st.granted) {
                auto loop = std::make_unique<Loopback>();
                if (loop->start()) {
                    mLoopback = std::move(loop);
                    log.info("Loopback running - speak, you should hear yourself");
                } else {
                    log.error("Microphone is allowed but the audio stream failed to open (see 'VoiceChat' log lines)");
                }
                return;
            }
            if (!st.declared) {
                log.error("{} does not declare the RECORD_AUDIO permission, so it can't be granted. "
                          "Voice chat needs a launcher build that declares it.", st.package);
                return;
            }
            if (mConfig.requestMicPermission && !requested) {
                requested = true;
                log.info("Requesting microphone permission: {}", requestMicPermission());
            }
        }
        if (!keepWaiting(std::chrono::seconds(2))) return; // disable() was called
    }
    log.warn("Gave up waiting for the microphone permission (allow it in Android Settings > Apps > LeviLauncher)");
}

bool VoiceMod::disable() {
    getSelf().getLogger().debug("Disabling...");
    {
        std::lock_guard<std::mutex> lock(mAudioMu);
        mAudioStop = true;
    }
    mAudioCv.notify_all();
    if (mAudioThread.joinable()) mAudioThread.join(); // after this nobody else touches mLoopback
    if (mTracker) {
        mTracker->stop();
        mTracker.reset();
    }
    if (mSocketLoop) {
        mSocketLoop->stop();
        mSocketLoop.reset();
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
