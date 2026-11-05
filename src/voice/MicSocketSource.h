#pragma once
// Receives microphone audio from the LeviLauncher app's VoiceCaptureService.
//
// Protocol (read out of the launcher's classes.dex):
//  * The MOD is the server: it listens on the Linux ABSTRACT unix socket
//    "org.levimc.voicechatlevi.audio" (address = '\0' + name). The launcher service
//    connects as a client, retrying until the socket exists.
//  * Payload: raw PCM, signed 16-bit, mono, 16 000 Hz, native (little) endian, no header.
//    The launcher writes 640-byte frames (320 samples = 20 ms), but we treat it as a
//    byte stream and re-frame it ourselves.
//  * The mod must NOT write anything back: the launcher reads up to 8 bytes after
//    connecting and "stands down" if they start with "VCLS".
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace voicechat {

class MicSocketSource {
  public:
    static constexpr int kSampleRate = 16000;
    static constexpr int kFrameSamples = 320; // 20 ms

    using FrameFn = std::function<void(const int16_t *samples, int count)>; // count == kFrameSamples
    using LogFn = std::function<void(const std::string &)>;

    MicSocketSource(std::string abstractName, FrameFn onFrame, LogFn log);
    ~MicSocketSource();

    bool start(); // bind + listen; false (and a log line) on failure
    void stop();  // idempotent; returns after the worker thread has exited

    bool connected() const { return connected_.load(); }
    std::uint64_t framesReceived() const { return frames_.load(); }

  private:
    void run();
    void serve(int client);

    std::string name_;
    FrameFn onFrame_;
    LogFn log_;
    int listenFd_ = -1;
    int wake_[2] = {-1, -1}; // pipe used to interrupt poll() on stop()
    std::atomic<bool> stop_{false};
    std::atomic<bool> connected_{false};
    std::atomic<std::uint64_t> frames_{0};
    std::thread thread_;
};

} // namespace voicechat
