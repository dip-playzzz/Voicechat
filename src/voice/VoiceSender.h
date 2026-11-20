#pragma once
// Microphone frames (16 kHz mono, 320 samples = 20 ms) -> gain -> voice gate -> Opus packet.
#include <atomic>
#include <cstdint>
#include <functional>

struct OpusEncoder;

namespace voicechat {

struct SenderConfig {
    int bitrate = 20000;        // bits/s; plenty for voice
    int micGainPercent = 100;   // 100 = unchanged
    int gateRms = 350;          // frames quieter than this are not sent (0 = always send)
    int hangoverFrames = 12;    // keep sending 240 ms after the voice drops below the gate
};

class VoiceSender {
  public:
    static constexpr int kFrame = 320;
    using EmitFn = std::function<void(const std::uint8_t *opus, int len)>;

    VoiceSender(SenderConfig cfg, EmitFn emit);
    ~VoiceSender();
    VoiceSender(const VoiceSender &) = delete;
    VoiceSender &operator=(const VoiceSender &) = delete;

    bool ok() const { return enc_ != nullptr; }
    void pushMic(const std::int16_t *pcm, int n); // n must be kFrame; one thread at a time
    void setGain(int percent);
    void setGate(int rms);
    bool speaking() const { return speaking_.load(); }
    int lastRms() const { return rms_.load(); }
    std::uint64_t framesSent() const { return sent_.load(); }

  private:
    SenderConfig cfg_;
    EmitFn emit_;
    OpusEncoder *enc_ = nullptr;
    std::atomic<int> gain_{100}, gate_{350}; // adjustable while running (Mod Menu sliders)
    int hang_ = 0;
    std::atomic<bool> speaking_{false};
    std::atomic<int> rms_{0};
    std::atomic<std::uint64_t> sent_{0};
};

} // namespace voicechat
