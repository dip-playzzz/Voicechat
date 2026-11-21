#pragma once
// Thin WebRTC APM wrapper. Playback and capture are always 16 kHz; APM uses
// 10 ms frames while the sender uses 20 ms frames. No method is called from
// an Oboe callback. Render and capture methods may run on separate workers.
#include <cstdint>
#include <memory>

namespace voicechat {

class WebRtcProcessor {
  public:
    static constexpr int kRate = 16000;
    static constexpr int kBlock = 160; // samples/channel, 10 ms
    static constexpr int kCaptureFrame = 2 * kBlock;

    struct Options {
        bool voiceEchoCancellation = false; // only the mod's voice output is available as reference
        bool noiseSuppression = false;
        int estimatedDelayMs = 50; // estimate, NOT measured hardware latency
    };

    explicit WebRtcProcessor(Options options);
    ~WebRtcProcessor();
    WebRtcProcessor(const WebRtcProcessor &) = delete;
    WebRtcProcessor &operator=(const WebRtcProcessor &) = delete;

    static bool compiledIn();
    bool ready() const;
    // Each render call is one 10 ms stereo reference block from the actual
    // mod output. Never use its output to replace the audible speaker data.
    bool processRender(const std::int16_t *interleavedStereo);
    // Capture is 20 ms mono. On failure callers should use the original frame.
    bool processCapture(const std::int16_t *input, std::int16_t *output);

  private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace voicechat
