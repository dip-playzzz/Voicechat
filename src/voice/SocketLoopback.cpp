#include "voice/SocketLoopback.h"

#include "voice/MicSocketSource.h"
#include "voice/RingBuffer.h"

#include <android/log.h>
#include <oboe/Oboe.h>
#include <opus.h>

#include <chrono>
#include <cmath>
#include <cstdlib>

#define TAG "VoiceChat"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace voicechat {

static constexpr int kRate = MicSocketSource::kSampleRate; // 16 kHz, same as the launcher's capture
static constexpr int kFrame = MicSocketSource::kFrameSamples;
static constexpr int kBitrate = 20000;
static constexpr int kMaxPacket = 400;
static constexpr const char *kSocketName = "org.levimc.voicechatlevi.audio";

struct SocketLoopback::Impl : oboe::AudioStreamDataCallback {
    LogFn log;
    std::shared_ptr<oboe::AudioStream> out;
    OpusEncoder *enc = nullptr;
    OpusDecoder *dec = nullptr;
    SpscRing<int16_t> playback{kRate}; // ~1 s; producer = socket thread, consumer = audio callback
    std::unique_ptr<MicSocketSource> source;

    // Level meter, touched only on the socket thread.
    std::chrono::steady_clock::time_point statStart = std::chrono::steady_clock::now();
    int statFrames = 0;
    int statPeak = 0;

    explicit Impl(LogFn l) : log(std::move(l)) {}

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream *, void *data, int32_t frames) override {
        auto *pcm = static_cast<int16_t *>(data);
        const size_t got = playback.read(pcm, frames);
        for (size_t i = got; i < static_cast<size_t>(frames); i++) pcm[i] = 0; // underrun -> silence
        return oboe::DataCallbackResult::Continue;
    }

    void onFrame(const int16_t *s, int n) {
        // Once every 5 s say how loud the incoming mic is, so "silent mic" is easy to spot.
        for (int i = 0; i < n; i++) statPeak = std::max(statPeak, std::abs(static_cast<int>(s[i])));
        statFrames++;
        const auto now = std::chrono::steady_clock::now();
        if (now - statStart >= std::chrono::seconds(5)) {
            const double db = statPeak > 0 ? 20.0 * std::log10(statPeak / 32768.0) : -99.0;
            if (log)
                log("mic stream: " + std::to_string(statFrames) + " frames in 5 s, peak " +
                    std::to_string(static_cast<int>(db)) + " dBFS" +
                    (db < -70.0 ? " (silent - is the mic blocked or the app in the background?)" : ""));
            statStart = now;
            statFrames = 0;
            statPeak = 0;
        }

        unsigned char packet[kMaxPacket];
        const int len = opus_encode(enc, s, n, packet, kMaxPacket);
        if (len <= 0) return;
        // TODO: send `packet` (len bytes) to the relay instead of decoding locally.
        int16_t decoded[kFrame];
        const int got = opus_decode(dec, packet, len, decoded, kFrame, 0);
        if (got > 0) playback.write(decoded, got);
    }
};

SocketLoopback::SocketLoopback(LogFn log) : d_(new Impl(std::move(log))) {}
SocketLoopback::~SocketLoopback() { stop(); }

bool SocketLoopback::start() {
    int err = 0;
    d_->enc = opus_encoder_create(kRate, 1, OPUS_APPLICATION_VOIP, &err);
    d_->dec = opus_decoder_create(kRate, 1, &err);
    if (!d_->enc || !d_->dec) {
        LOGE("opus init failed (%d)", err);
        stop();
        return false;
    }
    opus_encoder_ctl(d_->enc, OPUS_SET_BITRATE(kBitrate));
    opus_encoder_ctl(d_->enc, OPUS_SET_INBAND_FEC(1));

    oboe::AudioStreamBuilder b;
    b.setDirection(oboe::Direction::Output)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(oboe::SharingMode::Shared)
        ->setFormat(oboe::AudioFormat::I16)
        ->setChannelCount(oboe::ChannelCount::Mono)
        ->setSampleRate(kRate)
        ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
        ->setDataCallback(d_.get());
    const oboe::Result r = b.openStream(d_->out);
    if (r != oboe::Result::OK) {
        LOGE("open output failed: %s", oboe::convertToText(r));
        stop();
        return false;
    }
    d_->out->requestStart();

    Impl *impl = d_.get();
    d_->source = std::make_unique<MicSocketSource>(
        kSocketName, [impl](const int16_t *s, int n) { impl->onFrame(s, n); }, d_->log);
    if (!d_->source->start()) {
        stop();
        return false;
    }
    return true;
}

void SocketLoopback::stop() {
    if (d_->source) { d_->source->stop(); d_->source.reset(); } // joins the socket thread first
    if (d_->out) { d_->out->stop(); d_->out->close(); d_->out.reset(); }
    if (d_->enc) { opus_encoder_destroy(d_->enc); d_->enc = nullptr; }
    if (d_->dec) { opus_decoder_destroy(d_->dec); d_->dec = nullptr; }
}

} // namespace voicechat
