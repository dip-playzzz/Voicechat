#include "mod/VoiceMod.h"

#include <chrono>
#include <filesystem>
#include <string>

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

    // The card in LeviLauncher's Mod Menu (toggle + a few sliders). Missing menu = just no card.
    std::string why;
    if (mMenu.connect(
            self.getId(), menuState(), [this](bool enabled) { onMenuToggle(enabled); },
            [this](std::string_view key, std::string_view value) { onMenuChanged(key, value); }, &why)) {
        self.getLogger().info("Mod Menu: 'Proximity Voice Chat' card registered");
    } else {
        self.getLogger().warn("No Mod Menu card ({}). Settings still work from config.json.", why);
    }

    if (!mConfig.enabled) {
        self.getLogger().info("Voice chat is off (switch it on in the Mod Menu, or set enabled=true in config.json)");
        return true;
    }
    std::lock_guard<std::mutex> lock(mSvcMu);
    return startServices();
}

bool VoiceMod::startServices() {
    auto &self = getSelf();
    if (mServicesUp) return true;

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
            // ids below 2^31 only: the voice bridge plugin uses the top bit for its own (Java) players
            rc.playerId = (mConfig.playerId != 0 ? static_cast<std::uint32_t>(mConfig.playerId)
                                                 : std::random_device{}()) & 0x7fffffffu;
            if (rc.playerId == 0) rc.playerId = 1;
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
    mServicesUp = true;
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
    mMenu.disconnect(); // from here on the launcher can no longer call into us
    {
        std::lock_guard<std::mutex> lock(mReapplyMu);
        mReapplyWanted = false;
    }
    if (mReapplyThread.joinable()) mReapplyThread.join();
    std::lock_guard<std::mutex> lock(mSvcMu);
    stopServices();
    return true;
}

void VoiceMod::stopServices() {
    getSelf().getLogger().debug("Stopping voice services...");
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
    mServicesUp = false;
}

// ------------------------------------------------------------------ Mod Menu

MenuState VoiceMod::menuState() const {
    MenuState s;
    s.enabled = mConfig.enabled;
    s.useRelay = mConfig.useRelay;
    s.micGainPercent = mConfig.micGainPercent;
    s.micGate = mConfig.micGate;
    s.radius = mConfig.radius;
    return s;
}

void VoiceMod::saveConfig() {
    if (!mConfigFile) return;
    mConfigFile->value() = mConfig;
    if (!mConfigFile->save()) getSelf().getLogger().warn("Could not save config.json");
}

void VoiceMod::onMenuToggle(bool enabled) {
    if (mConfig.enabled == enabled) return;
    mConfig.enabled = enabled;
    saveConfig();
    getSelf().getLogger().info("Voice chat {} from the Mod Menu", enabled ? "ON" : "OFF");
    requestReapply();
}

void VoiceMod::onMenuChanged(std::string_view keyView, std::string_view valueView) {
    const std::string key(keyView), value(valueView);
    bool restart = false;
    if (key == "useRelay") {
        const bool v = ModMenuLink::parseBool(value, mConfig.useRelay);
        restart = v != mConfig.useRelay;
        mConfig.useRelay = v;
    } else if (key == "micGainPercent") {
        mConfig.micGainPercent = ModMenuLink::parseInt(value, mConfig.micGainPercent, 0, 400);
        std::unique_lock<std::mutex> lock(mSvcMu, std::try_to_lock); // don't stall the UI if services are restarting
        if (lock.owns_lock() && mSession) mSession->setMicGain(mConfig.micGainPercent);
    } else if (key == "micGate") {
        mConfig.micGate = ModMenuLink::parseInt(value, mConfig.micGate, 0, 2000);
        std::unique_lock<std::mutex> lock(mSvcMu, std::try_to_lock);
        if (lock.owns_lock() && mSession) mSession->setMicGate(mConfig.micGate);
    } else if (key == "radius") {
        mConfig.radius = ModMenuLink::parseInt(value, mConfig.radius, 8, 128);
        std::unique_lock<std::mutex> lock(mSvcMu, std::try_to_lock);
        if (lock.owns_lock() && mSession) mSession->setRadius(static_cast<float>(mConfig.radius));
    } else {
        return;
    }
    saveConfig();
    getSelf().getLogger().info("Mod Menu: {} = {}", key, value);
    if (restart) requestReapply();
}

void VoiceMod::requestReapply() {
    std::lock_guard<std::mutex> lock(mReapplyMu);
    mReapplyWanted = true;
    if (mReapplyRunning) return; // the running worker will see the new request
    if (mReapplyThread.joinable()) mReapplyThread.join(); // previous worker has finished
    mReapplyRunning = true;
    mReapplyThread = std::thread([this] {
        for (;;) {
            {
                std::lock_guard<std::mutex> l(mReapplyMu);
                if (!mReapplyWanted) { mReapplyRunning = false; return; }
                mReapplyWanted = false;
            }
            std::lock_guard<std::mutex> g(mSvcMu);
            stopServices();
            if (mConfig.enabled) startServices();
        }
    });
}

bool VoiceMod::unload() {
    getSelf().getLogger().debug("Unloading...");
    mConfigFile.reset();
    return true;
}

} // namespace voicechat
