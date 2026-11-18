#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "mod/Config.h"
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
    void audioStartupLoop();
    void relayPumpLoop();

    ll::mod::NativeMod &mSelf;
    ModConfig mConfig;
    std::optional<pl::config::ConfigFile<ModConfig>> mConfigFile;
    std::unique_ptr<Loopback> mLoopback;
    std::unique_ptr<SocketLoopback> mSocketLoop;
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
