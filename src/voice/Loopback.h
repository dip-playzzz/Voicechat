#pragma once
#include <memory>
#include <string>

#include "voice/AudioDefaults.h"

namespace voicechat {

// Mic -> Opus encode -> Opus decode -> speaker.
// Used as the Self Hearing path (and as the Resound-compatible fallback when
// the game mixer / FMOD world group is not available yet).
// Use headphones or you'll get feedback.
class Loopback {
  public:
    struct Options {
        // Android InputPreset name. Default voice_recognition (speech-first).
        // voice_communication often kills speech on some OEMs while keeping noise.
        std::string inputPreset = "voice_recognition";
        // Applied to mic samples before Opus encode (dB).
        float micGainDb = audio_defaults::selfHearingMicGainDb;
        // Playback gain for self-hearing (linear 0..2).
        float playbackVolume = audio_defaults::selfHearingVolume;
    };

    Loopback();
    ~Loopback();
    bool start();
    bool start(const Options &options);
    void stop();

    // 0.0 = silent, 1.0 = 100%, 2.0 = 200% (clamped).
    void setVolume(float linear);
    float volume() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace voicechat
