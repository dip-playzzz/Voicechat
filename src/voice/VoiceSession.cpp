#include "voice/VoiceSession.h"

#include "voice/MicSocketSource.h"
#include "voice/RingBuffer.h"
#include "voice/WebRtcProcessor.h"

#include <cstring> // Oboe 1.9's umbrella header uses memset
#include <android/log.h>
#include <oboe/Oboe.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
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
    std::atomic<bool> captureStop{false}, directRunning{false}, captureOverrun{false};
    std::mutex captureMu;
    std::condition_variable captureCv;
    std::thread captureThread;

    // One mic producer (socket or direct worker), one playback consumer.
    SpscRing<std::int16_t> monitor{1600}; // at most 100 ms of self-hearing
    const bool selfHearing;
    const float selfVolume;
    LogFn log;

    // The audio callback only copies the exact stereo samples it hands to
    // Oboe into fixed 10 ms blocks. A separate worker feeds the reverse stream
    // to APM; no WebRTC processing or allocation runs on the audio callback.
    struct RenderBlock { std::int16_t pcm[WebRtcProcessor::kBlock * 2]{}; };
    SpscRing<RenderBlock> renderReference{32}; // at most 320 ms, bounded
    RenderBlock renderPending{};              // output callback only
    int renderPendingFrames = 0;              // output callback only
    std::thread renderThread;
    std::condition_variable renderCv;
    std::mutex renderMu;
    std::atomic<bool> renderStop{false};
    std::atomic<bool> aecActive{false}, noiseActive{false}, processingHealthy{false}, renderOverrun{false};
    std::unique_ptr<WebRtcProcessor> processor; // lives until both workers and callbacks have stopped

    explicit Impl(const VoiceSessionConfig &c, LogFn logger)
        : remotes(c.mixer), selfHearing(c.selfHearing),
          selfVolume(std::isfinite(c.selfHearingVolume) ? std::clamp(c.selfHearingVolume, 0.f, 2.f) : 0.f),
          log(std::move(logger)) {}

    void processingFailure(const char *reason) {
        if (processingHealthy.exchange(false) && log)
            log(std::string("WebRTC processing stopped: ") + reason + "; using unprocessed microphone until restart");
    }

    void onMicFrame(const std::int16_t *samples, int count) {
        if (selfHearing) monitor.write(samples, static_cast<std::size_t>(count));
        if (!sender) return;
        std::array<std::int16_t, WebRtcProcessor::kCaptureFrame> processed{};
        if (processor && processingHealthy.load() && count == WebRtcProcessor::kCaptureFrame) {
            if (processor->processCapture(samples, processed.data())) {
                // A concurrent reference overrun makes this partial AEC result
                // unreliable; bypass rather than pretending it is aligned.
                if (processingHealthy.load()) samples = processed.data();
            } else {
                processingFailure("capture processing failed");
            }
        }
        sender->pushMic(samples, count);
    }

    void renderLoop() {
        RenderBlock block;
        while (!renderStop.load()) {
            if (renderOverrun.exchange(false) && log)
                log("WebRTC render reference overran; using unprocessed microphone until restart");
            while (renderReference.read(&block, 1) == 1) {
                if (processingHealthy.load() && !processor->processRender(block.pcm))
                    processingFailure("render reference processing failed");
            }
            // Do not notify from the real-time output callback. A short bounded
            // wait keeps the reference close to the audio HAL without blocking it.
            std::unique_lock<std::mutex> lock(renderMu);
            renderCv.wait_for(lock, std::chrono::milliseconds(5), [this] { return renderStop.load(); });
        }
    }

    void captureLoop() {
        std::array<std::int16_t, VoiceSender::kFrame> frame{};
        std::size_t fill = 0;
        while (!captureStop.load()) {
            if (captureOverrun.exchange(false) && log)
                log("WebRTC capture input overran; using unprocessed microphone until restart");
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
            const auto count = static_cast<std::size_t>(frames);
            if (capture.write(static_cast<const std::int16_t *>(data), count) != count &&
                processingHealthy.exchange(false)) {
                // Capture loss also breaks AEC timing. Report from the worker,
                // not this callback, and bypass processing until restart.
                captureOverrun.store(true);
            }
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
        if (aecActive.load(std::memory_order_relaxed) && processingHealthy.load(std::memory_order_relaxed)) {
            for (int offset = 0; offset < frames;) {
                const int n = std::min(WebRtcProcessor::kBlock - renderPendingFrames, frames - offset);
                std::memcpy(renderPending.pcm + 2 * renderPendingFrames, stereo + 2 * offset,
                            static_cast<std::size_t>(n) * 2 * sizeof(std::int16_t));
                renderPendingFrames += n;
                offset += n;
                if (renderPendingFrames == WebRtcProcessor::kBlock) {
                    if (renderReference.write(&renderPending, 1) != 1) {
                        // A dropped reference destroys timing. Stop processing
                        // rather than silently canceling against stale audio.
                        processingHealthy.store(false);
                        renderOverrun.store(true);
                        renderPendingFrames = 0;
                        break;
                    }
                    renderPendingFrames = 0;
                }
            }
        }
        return oboe::DataCallbackResult::Continue;
    }
};

VoiceSession::VoiceSession(VoiceSessionConfig cfg, PositionFn position, LogFn log)
    : d_(new Impl(cfg, log)), cfg_(std::move(cfg)), position_(std::move(position)), log_(std::move(log)) {}

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

    // The launcher already asks Android for AEC. Avoid adding a second AEC
    // there, or on the direct communication preset. Monitoring is also
    // excluded: this partial canceller must not be treated as feedback safety.
    bool voiceAec = cfg_.webrtcVoiceAec;
    if (voiceAec && cfg_.micSource != "direct") {
        voiceAec = false;
        if (log_) log_("WebRTC voice-only AEC skipped: launcher capture already requests Android AEC");
    }
    if (voiceAec && inputPreset(cfg_.micInputPreset) == oboe::InputPreset::VoiceCommunication) {
        voiceAec = false;
        if (log_) log_("WebRTC voice-only AEC skipped: the communication preset may already use platform AEC");
    }
    if (voiceAec && cfg_.selfHearing) {
        voiceAec = false;
        if (log_) log_("WebRTC voice-only AEC skipped while self-hearing is enabled; it is not feedback protection");
    }
    if (voiceAec || cfg_.webrtcNoiseSuppression) {
        if (!WebRtcProcessor::compiledIn()) {
            if (log_) log_("WebRTC processing is not included in this build; microphone remains unprocessed");
        } else {
            try {
                d->processor = std::make_unique<WebRtcProcessor>(WebRtcProcessor::Options{
                    voiceAec, cfg_.webrtcNoiseSuppression, cfg_.webrtcAecDelayMs});
                if (!d->processor->ready()) {
                    d->processor.reset();
                    if (log_) log_("WebRTC processor could not start; microphone remains unprocessed");
                } else {
                    d->renderOverrun = false;
                    d->processingHealthy = true;
                    d->aecActive = voiceAec;
                    d->noiseActive = cfg_.webrtcNoiseSuppression;
                    if (voiceAec) {
                        d->renderStop = false;
                        d->renderThread = std::thread([d] { d->renderLoop(); });
                        if (log_) log_("Experimental WebRTC AEC enabled for mod voice playback only; "
                                       "Minecraft game audio is NOT in its reference and the delay is estimated");
                    }
                    if (d->noiseActive && log_) log_("WebRTC moderate noise suppression enabled for multiplayer mic");
                }
            } catch (const std::exception &e) {
                d->processingHealthy = false;
                d->aecActive = false;
                d->noiseActive = false;
                d->renderStop = true;
                d->renderCv.notify_all();
                if (d->renderThread.joinable()) d->renderThread.join();
                d->processor.reset();
                if (log_) log_(std::string("WebRTC setup failed; microphone remains unprocessed: ") + e.what());
            }
        }
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
    d->captureOverrun = false;
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
    d->renderStop = true;
    d->renderCv.notify_all();
    if (d->renderThread.joinable()) d->renderThread.join();
    d->aecActive = false;
    d->noiseActive = false;
    d->processingHealthy = false;
    d->renderOverrun = false;
    d->captureOverrun = false;
    d->renderReference.clear();
    d->renderPendingFrames = 0;
    d->processor.reset();
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
    if (cfg_.webrtcVoiceAec || cfg_.webrtcNoiseSuppression) {
        if ((d->aecActive || d->noiseActive) && !d->processingHealthy) {
            s += ", WebRTC processing bypassed after error";
        } else {
            s += d->aecActive ? ", WebRTC voice-only AEC on" : ", WebRTC voice-only AEC off";
            s += d->noiseActive ? ", WebRTC NS on" : ", WebRTC NS off";
        }
    }
    s += ", speakers heard now " + std::to_string(d->remotes.speakerCount());
    return s;
}

} // namespace voicechat
