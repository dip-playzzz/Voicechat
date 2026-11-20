#pragma once
// Everything that arrives from the relay: per-speaker jitter buffer -> Opus decode (with loss
// concealment) -> distance volume + left/right pan -> one stereo stream for the speaker.
#include "voice/RelayClient.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

struct OpusDecoder;

namespace voicechat {

class RemoteVoices {
  public:
    static constexpr int kFrame = 320; // samples per 20 ms tick at 16 kHz

    struct Config {
        float radius = 32.f;   // blocks; beyond this a voice is silent
        int jitterFrames = 3;  // 60 ms of buffering before a voice starts playing
        int maxSpeakers = 16;
        int maxQueue = 25;     // frames kept per speaker
    };

    explicit RemoteVoices(Config cfg);
    ~RemoteVoices();
    RemoteVoices(const RemoteVoices &) = delete;
    RemoteVoices &operator=(const RemoteVoices &) = delete;

    void onPacket(const ForwardedVoice &v);                                         // network thread
    void setListener(bool valid, int dim, float x, float y, float z, float yawDeg); // any thread
    void render(std::int16_t *stereo, int frames);                                  // audio thread

    void setRadius(float blocks);
    int speakerCount() const;
    std::uint64_t lateDropped() const { std::lock_guard<std::mutex> lock(mu_); return late_; }
    std::uint64_t concealed() const { std::lock_guard<std::mutex> lock(mu_); return plc_; }

  private:
    struct Speaker {
        OpusDecoder *dec = nullptr;
        std::map<std::int64_t, std::vector<std::uint8_t>> q; // extended sequence -> opus packet
        std::int64_t lastExt = 0, next = 0;
        bool playing = false;
        int waited = 0, idle = 0;
        int dim = 0;
        float x = 0, y = 0, z = 0;
        std::chrono::steady_clock::time_point seen;
    };

    void tick(std::int16_t *out640); // mu_ held
    static constexpr int kFifo = kFrame * 2; // leftover of one stereo tick; never needs to grow

    Config cfg_;
    mutable std::mutex mu_;
    std::unordered_map<std::uint32_t, Speaker> speakers_;
    bool lValid_ = false;
    int lDim_ = 0;
    float lx_ = 0, ly_ = 0, lz_ = 0, lyaw_ = 0;
    std::int16_t fifo_[kFifo];
    int fifoCount_ = 0;
    std::uint64_t late_ = 0, plc_ = 0;
};

} // namespace voicechat
