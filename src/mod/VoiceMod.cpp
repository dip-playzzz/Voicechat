#include "mod/VoiceMod.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
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

    // Older configs only had loopbackTest. Read it before ConfigFile merges in
    // the new defaults, so upgrading does not unexpectedly turn self-hearing on.
    std::optional<bool> legacySelfHearing;
    try {
        std::ifstream stream(self.getConfigDir() / "config.json");
        if (stream) {
            nlohmann::json old;
            stream >> old;
            if (old.is_object() && !old.contains("selfHearing") && old.contains("loopbackTest") &&
                old["loopbackTest"].is_boolean())
                legacySelfHearing = old["loopbackTest"].get<bool>();
        }
    } catch (const std::exception &) {
        // ConfigFile handles malformed files and reports the error itself.
    }

    mConfigFile.emplace();
    if (!mConfigFile->load()) {
        self.getLogger().warn("Failed to load typed config");
        return false;
    }
    mConfig = mConfigFile->value();
    if (legacySelfHearing.has_value()) {
        mConfig.selfHearing = *legacySelfHearing;
        mConfigFile->value() = mConfig;
        if (!mConfigFile->save()) self.getLogger().warn("Could not save migrated self-hearing setting");
    }

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

    bool enabled;
    {
        std::lock_guard<std::mutex> lock(mConfigMu);
        enabled = mConfig.enabled;
    }
    if (!enabled) {
        self.getLogger().info("Voice chat is off (switch it on in the Mod Menu, or set enabled=true in config.json)");
        return true;
    }
    std::lock_guard<std::mutex> lock(mSvcMu);
    return startServices();
}

bool VoiceMod::startServices() {
    auto &self = getSelf();
    if (mServicesUp) return true;
    ModConfig config;
    {
        std::lock_guard<std::mutex> lock(mConfigMu);
        config = mConfig;
    }

    // Position offsets only match Minecraft 1.26.60 - 1.26.69. Never read other builds by default.
    const GameVersion version = parseGameVersion(self.getModDir().string());
    if (isSupportedGameVersion(version) || config.allowUnsupportedVersion) {
        self.getLogger().info("Minecraft {} - position tracking enabled", version.valid ? version.text : "(unknown)");
        mTracker = std::make_unique<PlayerTracker>(
            [this](const std::string &msg) { getSelf().getLogger().info("{}", msg); }, config.logPosition);
        mTracker->start();
    } else {
        self.getLogger().error(
            "Minecraft {} is not supported (position offsets are for 1.26.60 - 1.26.69). Position tracking is OFF "
            "to avoid crashing the game. Use Minecraft 1.26.6x with BedrockTools 1.5.12.",
            version.valid ? version.text : "(unknown)");
    }

    const bool sourceOk = config.micSource == "launcher" || config.micSource == "direct";
    if (!sourceOk) self.getLogger().error("Unknown micSource '{}': use 'launcher' or 'direct'", config.micSource);

    if (config.useRelay && sourceOk) {
        std::string error;
        if (config.secret.empty()) error = "'secret' is empty";
        else if (config.serverHost.empty()) error = "'serverHost' is empty";
        else if (config.serverPort < 1 || config.serverPort > 65535) error = "'serverPort' must be 1-65535";
        else if (config.playerId < 0) error = "'playerId' must be zero (random) or a positive integer";
        else if (config.room.empty() || config.room.size() > 64) error = "'room' must be 1-64 UTF-8 bytes";
        if (!error.empty()) {
            self.getLogger().error("Cannot start multiplayer: {}. Fix the mod config and restart voice services.", error);
        } else {
            RelayConfig rc;
            rc.host = config.serverHost;
            rc.port = static_cast<std::uint16_t>(config.serverPort);
            rc.room = config.room;
            rc.secret = config.secret;
            // The bridge reserves the top bit for Java players.
            rc.playerId = (config.playerId != 0 ? static_cast<std::uint32_t>(config.playerId)
                                                : std::random_device{}()) & 0x7fffffffu;
            if (rc.playerId == 0) rc.playerId = 1;
            if (rc.host == "127.0.0.1" || rc.host == "localhost" || rc.host == "::1")
                self.getLogger().warn("Relay host is localhost: it points to this phone, not another player's server");
            self.getLogger().info("Relay: joining {}:{} as player {} in room '{}'", rc.host, rc.port, rc.playerId, rc.room);

            VoiceSessionConfig vc;
            vc.mixer.radius = static_cast<float>(std::clamp(config.radius, 1, 512));
            vc.sender.micGainPercent = config.micGainPercent;
            vc.sender.gateRms = config.micGate;
            vc.micSource = config.micSource;
            vc.micInputPreset = config.micInputPreset;
            vc.selfHearing = config.selfHearing;
            vc.selfHearingVolume = config.selfHearingVolume;
            mSession = std::make_unique<VoiceSession>(
                vc, [this] { return mTracker ? mTracker->snapshot() : PlayerSnapshot{}; },
                [this](const std::string &msg) { getSelf().getLogger().info("{}", msg); });
            mRelay = std::make_unique<RelayClient>(
                rc, [this](const std::string &msg) { getSelf().getLogger().info("{}", msg); },
                [this](const ForwardedVoice &v) { if (mSession) mSession->onRemoteVoice(v); });
            mSession->attachRelay(mRelay.get());
            if (!mSession->start()) {
                self.getLogger().error("Could not start voice audio (see 'VoiceChat' log lines); relay connection will still be checked");
                mSession->attachRelay(nullptr);
                mSession.reset();
            } else {
                self.getLogger().info("Voice audio started. Waiting for relay, microphone and player position in room '{}'", rc.room);
            }
            mRelay->start();
            {
                std::lock_guard<std::mutex> lock(mRelayMu);
                mRelayStop = false;
            }
            mRelayThread = std::thread([this] { relayPumpLoop(); });
        }
    } else if (!config.useRelay) {
        self.getLogger().info("Multiplayer is off (set useRelay=true and configure the shared relay to talk to others)");
    }

    auto startPermissionWorker = [this, config](VoiceSession *session) {
        {
            std::lock_guard<std::mutex> lock(mAudioMu);
            mAudioStop = false;
        }
        // Never block the game/UI thread waiting for an Android permission prompt.
        mAudioThread = std::thread([this, config, session] { audioStartupLoop(config, session); });
    };

    if (mSession) {
        if (config.micSource == "direct") startPermissionWorker(mSession.get());
        // Self-hearing, if enabled, is mixed by this same session; do not open a second mic.
    } else if (config.selfHearing && sourceOk && config.micSource == "launcher") {
        mSocketLoop = std::make_unique<SocketLoopback>(
            [this](const std::string &msg) { getSelf().getLogger().info("{}", msg); });
        if (!mSocketLoop->start(config.selfHearingVolume)) {
            self.getLogger().error("Could not start the launcher-mic receiver (see lines above)");
            mSocketLoop.reset();
        } else {
            self.getLogger().info("Waiting for LeviLauncher's voice service (needs a launcher build with voice support)");
        }
    } else if (config.selfHearing && sourceOk && config.micSource == "direct") {
        startPermissionWorker(nullptr); // standalone direct self-hearing test
    }
    if (config.selfHearing && config.throughResound)
        self.getLogger().info("Self-hearing uses direct playback; the Resound/game-mixer path is not implemented");
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
        if (mRelay) {
            const PlayerSnapshot s = mTracker ? mTracker->snapshot() : PlayerSnapshot{};
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

void VoiceMod::audioStartupLoop(ModConfig config, VoiceSession *session) {
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
                if (session) {
                    if (session->startDirectMic())
                        log.info("Direct microphone capture running for multiplayer (preset={})", config.micInputPreset);
                    else
                        log.error("Microphone is allowed but direct capture failed (see 'VoiceChat' log lines)");
                } else {
                    Loopback::Options opt;
                    opt.inputPreset = config.micInputPreset;
                    opt.micGainDb = config.micGainDb;
                    opt.playbackVolume = config.selfHearingVolume;
                    auto loop = std::make_unique<Loopback>();
                    if (loop->start(opt)) {
                        mLoopback = std::move(loop);
                        log.info("Self Hearing running (direct loopback, preset={}, micGain={:.1f}dB). Use headphones.",
                                 config.micInputPreset, config.micGainDb);
                    } else {
                        log.error("Microphone is allowed but the audio stream failed to open (see 'VoiceChat' log lines)");
                    }
                }
                return;
            }
            if (!st.declared) {
                log.error("{} does not declare the RECORD_AUDIO permission, so it can't be granted. "
                          "Voice chat needs a launcher build that declares it.", st.package);
                return;
            }
            if (config.requestMicPermission && !requested) {
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
    {
        std::lock_guard<std::mutex> lock(mAudioMu);
        mAudioStop = true;
    }
    mAudioCv.notify_all();
    // The permission worker may be opening this session's direct microphone.
    if (mAudioThread.joinable()) mAudioThread.join();
    if (mSession) mSession->stop(); // no more mic/send activity after this
    if (mRelay) {
        mRelay->stop();             // no more voice callbacks after this
        mRelay.reset();
    }
    if (mSession) {
        mSession->attachRelay(nullptr);
        mSession.reset();
    }
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
    std::lock_guard<std::mutex> lock(mConfigMu);
    MenuState s;
    s.enabled = mConfig.enabled;
    s.useRelay = mConfig.useRelay;
    s.micGainPercent = mConfig.micGainPercent;
    s.micGate = mConfig.micGate;
    s.radius = mConfig.radius;
    return s;
}

void VoiceMod::saveConfigLocked() {
    if (!mConfigFile) return;
    mConfigFile->value() = mConfig;
    if (!mConfigFile->save()) getSelf().getLogger().warn("Could not save config.json");
}

void VoiceMod::onMenuToggle(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(mConfigMu);
        if (mConfig.enabled == enabled) return;
        mConfig.enabled = enabled;
        saveConfigLocked();
    }
    getSelf().getLogger().info("Voice chat {} from the Mod Menu", enabled ? "ON" : "OFF");
    requestReapply();
}

void VoiceMod::onMenuChanged(std::string_view keyView, std::string_view valueView) {
    const std::string key(keyView), value(valueView);
    bool restart = false;
    int setting = 0;
    {
        std::lock_guard<std::mutex> lock(mConfigMu);
        if (key == "useRelay") {
            const bool v = ModMenuLink::parseBool(value, mConfig.useRelay);
            restart = v != mConfig.useRelay;
            mConfig.useRelay = v;
        } else if (key == "micGainPercent") {
            setting = mConfig.micGainPercent = ModMenuLink::parseInt(value, mConfig.micGainPercent, 0, 400);
        } else if (key == "micGate") {
            setting = mConfig.micGate = ModMenuLink::parseInt(value, mConfig.micGate, 0, 2000);
        } else if (key == "radius") {
            setting = mConfig.radius = ModMenuLink::parseInt(value, mConfig.radius, 8, 128);
        } else {
            return;
        }
        saveConfigLocked();
    }
    if (key != "useRelay") {
        std::unique_lock<std::mutex> lock(mSvcMu, std::try_to_lock);
        if (!lock.owns_lock()) {
            // A restart already in progress may have taken an older config snapshot.
            restart = true;
        } else if (mSession) {
            // Read the latest value again after taking mSvcMu. Two UI callbacks can
            // otherwise apply their saved values in the opposite order.
            {
                std::lock_guard<std::mutex> configLock(mConfigMu);
                if (key == "micGainPercent") setting = mConfig.micGainPercent;
                else if (key == "micGate") setting = mConfig.micGate;
                else if (key == "radius") setting = mConfig.radius;
            }
            if (key == "micGainPercent") mSession->setMicGain(setting);
            else if (key == "micGate") mSession->setMicGate(setting);
            else if (key == "radius") mSession->setRadius(static_cast<float>(setting));
        }
    }
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
            bool enabled;
            {
                std::lock_guard<std::mutex> lock(mConfigMu);
                enabled = mConfig.enabled;
            }
            if (enabled) startServices();
        }
    });
}

bool VoiceMod::unload() {
    getSelf().getLogger().debug("Unloading...");
    std::lock_guard<std::mutex> lock(mConfigMu);
    mConfigFile.reset();
    return true;
}

} // namespace voicechat
