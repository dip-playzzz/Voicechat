#pragma once
// One voice-chat session on the phone:
//   launcher or direct mic -> VoiceSender (gate + Opus) -> relay (+ player position)
//   relay -> RemoteVoices (jitter, decode, distance + pan) -> stereo speaker
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
    std::string micSource = "launcher"; // launcher service, or direct Oboe capture
    std::string micInputPreset = "voice_recognition";
    std::string micSocketName = "org.levimc.voicechatlevi.audio";
    bool selfHearing = true;
    float selfHearingVolume = 1.0f;
};

class VoiceSession {
  public:
    using LogFn = std::function<void(const std::string &)>;
    using PositionFn = std::function<PlayerSnapshot()>;

    VoiceSession(VoiceSessionConfig cfg, PositionFn position, LogFn log);
    ~VoiceSession();

    // Opens playback and, for micSource=launcher, the launcher's microphone socket.
    bool start();
    // For micSource=direct, call after Android microphone permission is granted.
    // The owner must join the permission worker before calling stop().
    bool startDirectMic();
    void stop(); // idempotent

    void attachRelay(RelayClient *relay) { relay_.store(relay); } // nullptr detaches
    void onRemoteVoice(const ForwardedVoice &v);                  // relay worker thread
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
