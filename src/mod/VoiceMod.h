#pragma once

#include <memory>
#include <optional>

#include "mod/Config.h"
#include "voice/Loopback.h"
#include "voice/PlayerTracker.h"

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
    ll::mod::NativeMod &mSelf;
    ModConfig mConfig;
    std::optional<pl::config::ConfigFile<ModConfig>> mConfigFile;
    std::unique_ptr<Loopback> mLoopback;
    std::unique_ptr<PlayerTracker> mTracker;
};

} // namespace voicechat
