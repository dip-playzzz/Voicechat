#pragma once
// Launcher-mic path: microphone audio arrives from LeviLauncher's VoiceCaptureService over
// an abstract unix socket (see MicSocketSource.h), goes through Opus encode/decode, and is
// played back (self-hearing test). Later, the Opus packet is what gets sent to the relay.
#include <functional>
#include <memory>
#include <string>

namespace voicechat {

class SocketLoopback {
  public:
    using LogFn = std::function<void(const std::string &)>;
    explicit SocketLoopback(LogFn log);
    ~SocketLoopback();
    bool start();
    void stop();

  private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace voicechat
