#include "mod/VoiceMod.h"

#include <chrono>
#include <filesystem>

#include "voice/AndroidPermission.h"
#include "voice/GameVersion.h"

#include <random>

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

    // Position tracking reads game memory at fixed offsets that only match Minecraft 1.26.60 - 1.26.69.
    // On any other build that can crash the game, so refuse to start it there.
    const GameVersion version = parseGameVersion(self.getModDir().string());
    if (isSupportedGameVersion(version) || mConfig.allowUnsupportedVersion) {
        self.getLogger().info("Minecraft {} - position tracking enabled", version.valid ? version.text : "(unknown)");
        mTracker = std::make_unique<PlayerTracker>(
            [this](const std::string &msg) { getSelf().getLogger().info("{}", msg); }, mConfig.logPosition);
        mTracker->start();
    } else {
        self.getLogger().error(
            "Minecraft {} is not supported (position offsets are for 1.26.60 - 1.26.69). Position tracking is OFF "
            "to avoid crashing the game. Use Minecraft 1.26.6x with BedrockTools 1.5.12.",
            version.valid ? version.text : "(unknown)");
    }

    if (mConfig.useRelay) {
        if (mConfig.secret.empty()) {
            self.getLogger().error("useRelay is on but 'secret' is empty - set the room secret in the mod config");
        } else {
            RelayConfig rc;
            rc.host = mConfig.serverHost;
            rc.port = static_cast<std::uint16_t>(mConfig.serverPort);
            rc.room = mConfig.room;
            rc.secret = mConfig.secret;
            rc.playerId = mConfig.playerId != 0 ? static_cast<std::uint32_t>(mConfig.playerId)
                                                : (std::random_device{}() | 1u);
            self.getLogger().info("Relay: joining {}:{} as player {} in room '{}'", rc.host, rc.port, rc.playerId, rc.room);

            // Full voice needs the launcher's mic stream; with micSource=direct only the position link runs.
            if (mConfig.micSource == "launcher") {
                VoiceSessionConfig vc;
                vc.mixer.radius = static_cast<float>(mConfig.radius);
                vc.sender.micGainPercent = mConfig.micGainPercent;
                vc.sender.gateRms = mConfig.micGate;
                mSession = std::make_unique<VoiceSession>(
                    vc, [this] { return mTracker ? mTracker->snapshot() : PlayerSnapshot{}; },
                    [this](const std::string &msg) { getSelf().getLogger().info("{}", msg); });
            } else {
                self.getLogger().warn("Relay voice currently needs micSource=\"launcher\"; with \"direct\" only the position link runs");
            }
            mRelay = std::make_unique<RelayClient>(
                rc, [this](const std::string &msg) { getSelf().getLogger().info("{}", msg); },
                [this](const ForwardedVoice &v) { if (mSession) mSession->onRemoteVoice(v); });
            if (mSession) {
                mSession->attachRelay(mRelay.get());
                if (!mSession->start()) {
                    self.getLogger().error("Could not start voice (see lines above); position link still runs");
                    mSession->attachRelay(nullptr);
                    mSession.reset();
                } else {
                    self.getLogger().info("Voice chat ready: talk and you'll be heard by players in room '{}' within {} blocks", rc.room, mConfig.radius);
                }
            }
            mRelay->start();
            {
                std::lock_guard<std::mutex> lock(mRelayMu);
                mRelayStop = false;
            }
            mRelayThread = std::thread([this] { relayPumpLoop(); });
        }
    }

    if (mSession) {
        // relay voice owns the microphone stream; the self-hearing loopback would fight it for the socket
    } else if (mConfig.loopbackTest && mConfig.micSource == "launcher") {
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

void VoiceMod::relayPumpLoop() {
    // Sends the player's position ~10x/s (also keeps the NAT mapping alive) and logs a status line.
    auto &log = getSelf().getLogger();
    auto lastStatus = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lk(mRelayMu);
    while (!mRelayStop) {
        lk.unlock();
        if (mTracker && mRelay) {
            const PlayerSnapshot s = mTracker->snapshot();
            if (s.valid) mRelay->sendPosition(s.dimension, s.x, s.y, s.z, s.yaw);
            if (mSession) mSession->setListener(s);
        }
        if (mRelay && std::chrono::steady_clock::now() - lastStatus >= std::chrono::seconds(10)) {
            lastStatus = std::chrono::steady_clock::now();
            log.info("Relay: {} | voice packets heard so far: {}{}{}",
                     mRelay->connected() ? "connected" : "NOT connected", mRelay->voiceReceived(),
                     mSession ? " | " : "", mSession ? mSession->status() : std::string());
        }
        lk.lock();
        mRelayCv.wait_for(lk, std::chrono::milliseconds(100), [this] { return mRelayStop; });
    }
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
        std::lock_guard<std::mutex> lock(mRelayMu);
        mRelayStop = true;
    }
    mRelayCv.notify_all();
    if (mRelayThread.joinable()) mRelayThread.join();
    if (mSession) mSession->stop(); // no more mic/send activity after this
    if (mRelay) {
        mRelay->stop();             // no more voice callbacks after this
        mRelay.reset();
    }
    if (mSession) {
        mSession->attachRelay(nullptr);
        mSession.reset();
    }
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
