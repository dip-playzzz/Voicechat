#pragma once
// Host-only Oboe stand-in. Tests drive callbacks explicitly; the Android build
// uses the real Oboe library and does not include this directory.
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace oboe {
enum class Direction { Input, Output };
enum class PerformanceMode { LowLatency };
enum class SharingMode { Shared };
enum class AudioFormat { I16 };
enum class ChannelCount { Mono = 1, Stereo = 2 };
enum class SampleRateConversionQuality { Medium };
enum class InputPreset { VoiceRecognition, VoiceCommunication, Generic, Unprocessed, Camcorder, VoicePerformance };
enum class Result { OK, Error };
enum class DataCallbackResult { Continue, Stop };
inline const char *convertToText(Result r) { return r == Result::OK ? "OK" : "Error"; }

class AudioStream;
class AudioStreamDataCallback {
  public:
    virtual ~AudioStreamDataCallback() = default;
    virtual DataCallbackResult onAudioReady(AudioStream *, void *, int32_t) = 0;
};

class AudioStream {
  public:
    AudioStream(Direction direction, AudioStreamDataCallback *callback) : direction_(direction), callback_(callback) {}
    Direction getDirection() const { return direction_; }
    Result requestStart() { started_ = true; return Result::OK; }
    Result stop() { started_ = false; return Result::OK; }
    Result close() { started_ = false; return Result::OK; }
    bool testStarted() const { return started_.load(); }
    void testCallback(void *data, int32_t frames) {
        if (started_ && callback_) callback_->onAudioReady(this, data, frames);
    }
  private:
    Direction direction_;
    AudioStreamDataCallback *callback_;
    std::atomic<bool> started_{false};
};

namespace test {
inline std::mutex mu;
inline std::vector<std::weak_ptr<AudioStream>> streams;
inline std::vector<std::shared_ptr<AudioStream>> running(Direction direction) {
    std::vector<std::shared_ptr<AudioStream>> result;
    std::lock_guard<std::mutex> lock(mu);
    for (auto it = streams.begin(); it != streams.end();) {
        if (auto stream = it->lock()) {
            if (stream->getDirection() == direction && stream->testStarted()) result.push_back(stream);
            ++it;
        } else {
            it = streams.erase(it);
        }
    }
    return result;
}
} // namespace test

class AudioStreamBuilder {
  public:
    AudioStreamBuilder *setDirection(Direction v) { direction_ = v; return this; }
    AudioStreamBuilder *setPerformanceMode(PerformanceMode) { return this; }
    AudioStreamBuilder *setSharingMode(SharingMode) { return this; }
    AudioStreamBuilder *setFormat(AudioFormat) { return this; }
    AudioStreamBuilder *setChannelCount(ChannelCount) { return this; }
    AudioStreamBuilder *setSampleRate(int) { return this; }
    AudioStreamBuilder *setSampleRateConversionQuality(SampleRateConversionQuality) { return this; }
    AudioStreamBuilder *setInputPreset(InputPreset) { return this; }
    AudioStreamBuilder *setDataCallback(AudioStreamDataCallback *v) { callback_ = v; return this; }
    Result openStream(std::shared_ptr<AudioStream> &out) {
        out = std::make_shared<AudioStream>(direction_, callback_);
        std::lock_guard<std::mutex> lock(test::mu);
        test::streams.push_back(out);
        return Result::OK;
    }
  private:
    Direction direction_ = Direction::Output;
    AudioStreamDataCallback *callback_ = nullptr;
};
} // namespace oboe
