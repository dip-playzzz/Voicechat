#pragma once
// One voice-chat session on the phone:
//   launcher mic stream -> VoiceSender (gate + Opus) -> RelayClient.sendVoice(+ player position)
//   RelayClient voice   -> RemoteVoices (jitter, decode, distance + pan) -> stereo speaker (Oboe)
#include "voice/PlayerTracker.h"
#include "voice/RelayClient.h"
#include "voice/RemoteVoices.h"
#include "voice/VoiceSender.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace voicechat {

struct VoiceSessionConfig {
    RemoteVoices::Config mixer;
    SenderConfig sender;
    std::string micSocketName = "org.levimc.voicechatlevi.audio";
};

class VoiceSession {
  public:
    using LogFn = std::function<void(const std::string &)>;
    using PositionFn = std::function<PlayerSnapshot()>;

    VoiceSession(VoiceSessionConfig cfg, PositionFn position, LogFn log);
    ~VoiceSession();

    bool start(); // opens the speaker and starts listening for the launcher's mic stream
    void stop();  // idempotent

    void attachRelay(RelayClient *relay) { relay_.store(relay); } // nullptr detaches
    void onRemoteVoice(const ForwardedVoice &v);                  // RelayClient's voice callback
    void setListener(const PlayerSnapshot &s);                    // call ~10x/s with the local player
    void setMicGain(int percent);
    void setMicGate(int rms);
    void setRadius(float blocks);
    std::string status() const;

  private:
    struct Impl;
    std::atomic<RelayClient *> relay_{nullptr};
    std::unique_ptr<Impl> d_;
    VoiceSessionConfig cfg_;
    PositionFn position_;
    LogFn log_;
};

} // namespace voicechat
