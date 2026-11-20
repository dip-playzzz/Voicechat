#include "voice/VoiceSession.h"

#include "voice/MicSocketSource.h"
#include "voice/RingBuffer.h"

#include <cstring> // Oboe 1.9's umbrella header uses memset
#include <android/log.h>
#include <oboe/Oboe.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

#define TAG "VoiceChat"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace voicechat {

namespace {
oboe::InputPreset inputPreset(const std::string &name) {
    if (name == "voice_communication" || name == "communication") return oboe::InputPreset::VoiceCommunication;
    if (name == "generic") return oboe::InputPreset::Generic;
    if (name == "unprocessed") return oboe::InputPreset::Unprocessed;
    if (name == "camcorder") return oboe::InputPreset::Camcorder;
    if (name == "voice_performance" || name == "performance") return oboe::InputPreset::VoicePerformance;
    return oboe::InputPreset::VoiceRecognition;
}
} // namespace

struct VoiceSession::Impl : oboe::AudioStreamDataCallback {
    RemoteVoices remotes;
    std::unique_ptr<VoiceSender> sender;
    std::unique_ptr<MicSocketSource> mic;
    std::shared_ptr<oboe::AudioStream> in, out;
    std::atomic<std::uint16_t> seq{0};
    std::atomic<int> noPosition{0};
    std::atomic<std::uint64_t> packetsSent{0};

    // The Oboe input callback only copies samples. Encoding, position reads and UDP
    // sends run on a worker, never on Android's real-time audio callback.
    SpscRing<std::int16_t> capture{16000};
    std::atomic<bool> captureStop{false}, directRunning{false};
    std::mutex captureMu;
    std::condition_variable captureCv;
    std::thread captureThread;

    // One mic producer (socket or direct worker), one playback consumer.
    SpscRing<std::int16_t> monitor{1600}; // at most 100 ms of self-hearing
    const bool selfHearing;
    const float selfVolume;

    explicit Impl(const VoiceSessionConfig &c)
        : remotes(c.mixer), selfHearing(c.selfHearing),
          selfVolume(std::isfinite(c.selfHearingVolume) ? std::clamp(c.selfHearingVolume, 0.f, 2.f) : 1.f) {}

    void onMicFrame(const std::int16_t *samples, int count) {
        if (selfHearing) monitor.write(samples, static_cast<std::size_t>(count));
        if (sender) sender->pushMic(samples, count);
    }

    void captureLoop() {
        std::array<std::int16_t, VoiceSender::kFrame> frame{};
        std::size_t fill = 0;
        while (!captureStop.load()) {
            fill += capture.read(frame.data() + fill, frame.size() - fill);
            if (fill == frame.size()) {
                onMicFrame(frame.data(), static_cast<int>(fill));
                fill = 0;
                continue;
            }
            std::unique_lock<std::mutex> lock(captureMu);
            // A notification can race with going to sleep; the timeout bounds that delay.
            captureCv.wait_for(lock, std::chrono::milliseconds(20));
        }
    }

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream *stream, void *data, int32_t frames) override {
        if (frames <= 0) return oboe::DataCallbackResult::Continue;
        if (stream->getDirection() == oboe::Direction::Input) {
            capture.write(static_cast<const std::int16_t *>(data), static_cast<std::size_t>(frames));
            captureCv.notify_one();
            return oboe::DataCallbackResult::Continue;
        }

        auto *stereo = static_cast<std::int16_t *>(data);
        remotes.render(stereo, frames);
        if (selfHearing && selfVolume > 0.f) {
            std::int16_t local[VoiceSender::kFrame];
            for (int offset = 0; offset < frames;) {
                const int chunk = std::min(VoiceSender::kFrame, frames - offset);
                const auto got = monitor.read(local, static_cast<std::size_t>(chunk));
                for (std::size_t i = 0; i < got; i++) {
                    const int sample = static_cast<int>(local[i] * selfVolume);
                    for (int channel = 0; channel < 2; channel++) {
                        const int ix = 2 * (offset + static_cast<int>(i)) + channel;
                        stereo[ix] = static_cast<std::int16_t>(std::clamp(static_cast<int>(stereo[ix]) + sample, -32768, 32767));
                    }
                }
                offset += chunk;
            }
        }
        return oboe::DataCallbackResult::Continue;
    }
};

VoiceSession::VoiceSession(VoiceSessionConfig cfg, PositionFn position, LogFn log)
    : d_(new Impl(cfg)), cfg_(std::move(cfg)), position_(std::move(position)), log_(std::move(log)) {}

VoiceSession::~VoiceSession() { stop(); }

bool VoiceSession::start() {
    Impl *d = d_.get();
    if (d->out) return true;
    if (cfg_.micSource != "launcher" && cfg_.micSource != "direct") {
        if (log_) log_("Unknown micSource: use 'launcher' or 'direct'");
        return false;
    }

    d->sender = std::make_unique<VoiceSender>(cfg_.sender, [this, d](const std::uint8_t *opus, int n) {
        RelayClient *relay = relay_.load();
        if (!relay) return;
        const PlayerSnapshot s = position_ ? position_() : PlayerSnapshot{};
        if (!s.valid) {
            if (d->noPosition++ % 500 == 0 && log_)
                log_("Not sending voice: no player position yet (join a world; BedrockTools must be enabled "
                     "and Minecraft must be a supported version)");
            return;
        }
        if (relay->sendVoice(d->seq++, s.dimension, s.x, s.y, s.z, s.yaw, opus, n)) d->packetsSent++;
    });
    if (!d->sender->ok()) {
        LOGE("opus encoder failed to initialise");
        stop();
        return false;
    }

    oboe::AudioStreamBuilder b;
    b.setDirection(oboe::Direction::Output)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(oboe::SharingMode::Shared)
        ->setFormat(oboe::AudioFormat::I16)
        ->setChannelCount(oboe::ChannelCount::Stereo)
        ->setSampleRate(16000)
        ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
        ->setDataCallback(d);
    oboe::Result r = b.openStream(d->out);
    if (r != oboe::Result::OK) {
        LOGE("open speaker stream failed: %s", oboe::convertToText(r));
        stop();
        return false;
    }
    r = d->out->requestStart();
    if (r != oboe::Result::OK) {
        LOGE("start speaker stream failed: %s", oboe::convertToText(r));
        stop();
        return false;
    }

    if (cfg_.micSource == "launcher") {
        d->mic = std::make_unique<MicSocketSource>(
            cfg_.micSocketName, [d](const std::int16_t *s, int n) { d->onMicFrame(s, n); }, log_);
        if (!d->mic->start()) {
            stop();
            return false;
        }
    }
    return true;
}

bool VoiceSession::startDirectMic() {
    Impl *d = d_.get();
    if (cfg_.micSource != "direct" || !d->sender || !d->out) return false;
    if (d->directRunning.load()) return true;

    oboe::AudioStreamBuilder b;
    b.setDirection(oboe::Direction::Input)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(oboe::SharingMode::Shared)
        ->setFormat(oboe::AudioFormat::I16)
        ->setChannelCount(oboe::ChannelCount::Mono)
        ->setSampleRate(16000)
        ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
        ->setInputPreset(inputPreset(cfg_.micInputPreset))
        ->setDataCallback(d);
    oboe::Result r = b.openStream(d->in);
    if (r != oboe::Result::OK) {
        LOGE("open microphone stream failed: %s", oboe::convertToText(r));
        d->in.reset();
        return false;
    }
    d->capture.clear(); // no input callback or consumer is active yet
    d->captureStop = false;
    d->captureThread = std::thread([d] { d->captureLoop(); });
    r = d->in->requestStart();
    if (r != oboe::Result::OK) {
        LOGE("start microphone stream failed: %s", oboe::convertToText(r));
        d->captureStop = true;
        d->captureCv.notify_all();
        d->captureThread.join();
        d->in->close();
        d->in.reset();
        return false;
    }
    d->directRunning = true;
    return true;
}

void VoiceSession::stop() {
    Impl *d = d_.get();
    if (d->mic) { d->mic->stop(); d->mic.reset(); } // joins socket producer
    d->captureStop = true;
    if (d->in) { d->in->stop(); d->in->close(); d->in.reset(); } // ends Oboe callbacks
    d->captureCv.notify_all();
    if (d->captureThread.joinable()) d->captureThread.join();
    d->directRunning = false;
    if (d->out) { d->out->stop(); d->out->close(); d->out.reset(); }
    d->capture.clear();
    d->monitor.clear();
    d->remotes.setListener(false, 0, 0, 0, 0, 0);
    d->sender.reset();
}

void VoiceSession::onRemoteVoice(const ForwardedVoice &v) { d_->remotes.onPacket(v); }

void VoiceSession::setListener(const PlayerSnapshot &s) {
    d_->remotes.setListener(s.valid, s.dimension, s.x, s.y, s.z, s.yaw);
}

void VoiceSession::setMicGain(int percent) {
    if (d_->sender) d_->sender->setGain(percent);
}
void VoiceSession::setMicGate(int rms) {
    if (d_->sender) d_->sender->setGate(rms);
}
void VoiceSession::setRadius(float blocks) { d_->remotes.setRadius(blocks); }

std::string VoiceSession::status() const {
    const Impl *d = d_.get();
    std::string s = "mic ";
    if (cfg_.micSource == "direct") s += d->directRunning ? "direct capture running" : "waiting for permission or input";
    else s += (d->mic && d->mic->connected()) ? "connected" : "waiting for LeviLauncher's voice service";
    if (d->sender) {
        s += ", relay packets sent " + std::to_string(d->packetsSent.load());
        s += ", mic level " + std::to_string(d->sender->lastRms());
        s += d->sender->speaking() ? " (speaking)" : "";
    }
    s += ", speakers heard now " + std::to_string(d->remotes.speakerCount());
    return s;
}

} // namespace voicechat
