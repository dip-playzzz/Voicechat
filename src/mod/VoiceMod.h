#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

#include "mod/Config.h"
#include "mod/ModMenuLink.h"
#include "voice/Loopback.h"
#include "voice/PlayerTracker.h"
#include "voice/RelayClient.h"
#include "voice/VoiceSession.h"
#include "voice/SocketLoopback.h"

#include <pl/Mod.hpp>

namespace voicechat {

class VoiceMod {
  public:
    static VoiceMod &instance();

    VoiceMod();

    [[nodiscard]] ll::mod::NativeMod &getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();
    bool unload();

  private:
    bool startServices();   // tracker, relay, voice session / loopback (caller holds mSvcMu)
    void stopServices();    // everything above; safe to call when nothing is running
    void requestReapply();  // stop + start again on a worker thread (never blocks the launcher's UI)
    void onMenuToggle(bool enabled);
    void onMenuChanged(std::string_view key, std::string_view value);
    MenuState menuState() const;
    void saveConfigLocked(); // caller holds mConfigMu
    void audioStartupLoop(ModConfig config, VoiceSession *session);
    void relayPumpLoop();

    ll::mod::NativeMod &mSelf;
    mutable std::mutex mConfigMu;
    ModConfig mConfig; // guarded by mConfigMu once the menu is connected
    std::optional<pl::config::ConfigFile<ModConfig>> mConfigFile;
    bool mLoaded = false; // lifecycle thread; never enable after a failed load
    std::unique_ptr<Loopback> mLoopback;
    std::unique_ptr<SocketLoopback> mSocketLoop;
    ModMenuLink mMenu;
    std::mutex mSvcMu;          // serialises start/stop of the services
    bool mServicesUp = false;   // guarded by mSvcMu
    std::mutex mReapplyMu;
    bool mReapplyWanted = false, mReapplyRunning = false; // guarded by mReapplyMu
    std::thread mReapplyThread;
    std::unique_ptr<PlayerTracker> mTracker;
    std::unique_ptr<VoiceSession> mSession;
    std::unique_ptr<RelayClient> mRelay;
    std::thread mRelayThread;
    std::mutex mRelayMu;
    std::condition_variable mRelayCv;
    bool mRelayStop = false; // guarded by mRelayMu
    std::thread mAudioThread;
    std::mutex mAudioMu;
    std::condition_variable mAudioCv;
    bool mAudioStop = false; // guarded by mAudioMu
};

} // namespace voicechat
