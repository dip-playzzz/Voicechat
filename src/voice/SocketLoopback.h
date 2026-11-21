#pragma once
// Launcher-mic path: microphone audio arrives from LeviLauncher's VoiceCaptureService over
// an abstract unix socket (see MicSocketSource.h), goes through Opus encode/decode, and is
// played back as a standalone self-hearing test when multiplayer is off.
#include <functional>
#include <memory>
#include <string>

#include "voice/AudioDefaults.h"

namespace voicechat {

class SocketLoopback {
  public:
    using LogFn = std::function<void(const std::string &)>;
    explicit SocketLoopback(LogFn log);
    ~SocketLoopback();
    bool start(float volume = audio_defaults::selfHearingVolume);
    void stop();

  private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace voicechat
