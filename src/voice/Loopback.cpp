#include "voice/Loopback.h"

#include "voice/RingBuffer.h"

#include <android/log.h>
#include <oboe/Oboe.h>
#include <opus.h>

#include <vector>

#define TAG "VoiceChat"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace voicechat {

static constexpr int kRate = 48000;
static constexpr int kFrame = 960;     // 20 ms @ 48 kHz, mono
static constexpr int kBitrate = 24000; // plenty for voice
static constexpr int kMaxPacket = 400;

struct Loopback::Impl : oboe::AudioStreamDataCallback {
    std::shared_ptr<oboe::AudioStream> in, out;
    OpusEncoder *enc = nullptr;
    OpusDecoder *dec = nullptr;
    SpscRing<int16_t> playback{kRate}; // ~1 s of decoded audio
    std::vector<int16_t> pending;      // mic samples waiting to fill a frame

    Impl() { pending.reserve(kFrame * 8); }

    // One callback class serves both streams; tell them apart by direction.
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream *s, void *data, int32_t frames) override {
        if (s->getDirection() == oboe::Direction::Input) {
            auto *pcm = static_cast<const int16_t *>(data);
            pending.insert(pending.end(), pcm, pcm + frames);
            while (pending.size() >= static_cast<size_t>(kFrame)) {
                unsigned char packet[kMaxPacket];
                int n = opus_encode(enc, pending.data(), kFrame, packet, kMaxPacket);
                if (n > 0) {
                    // TODO: send `packet` (n bytes) to the relay instead of decoding locally.
                    int16_t decoded[kFrame];
                    int got = opus_decode(dec, packet, n, decoded, kFrame, 0);
                    if (got > 0) playback.write(decoded, got);
                }
                pending.erase(pending.begin(), pending.begin() + kFrame);
            }
        } else {
            auto *pcm = static_cast<int16_t *>(data);
            size_t got = playback.read(pcm, frames);
            for (size_t i = got; i < static_cast<size_t>(frames); i++) pcm[i] = 0; // underrun -> silence
        }
        return oboe::DataCallbackResult::Continue;
    }

    bool open(oboe::Direction dir, std::shared_ptr<oboe::AudioStream> &stream) {
        oboe::AudioStreamBuilder b;
        b.setDirection(dir)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(oboe::SharingMode::Shared)
            ->setFormat(oboe::AudioFormat::I16)
            ->setChannelCount(oboe::ChannelCount::Mono)
            ->setSampleRate(kRate)
            ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
            ->setDataCallback(this);
        if (dir == oboe::Direction::Input) b.setInputPreset(oboe::InputPreset::VoiceCommunication);
        oboe::Result r = b.openStream(stream);
        if (r != oboe::Result::OK) {
            LOGE("open %s failed: %s", dir == oboe::Direction::Input ? "input" : "output",
                 oboe::convertToText(r));
            return false;
        }
        return true;
    }
};

Loopback::Loopback() : d_(new Impl) {}
Loopback::~Loopback() { stop(); }

bool Loopback::start() {
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

    if (!d_->open(oboe::Direction::Output, d_->out) || !d_->open(oboe::Direction::Input, d_->in)) {
        stop();
        return false;
    }
    d_->out->requestStart();
    d_->in->requestStart();
    return true;
}

void Loopback::stop() {
    if (d_->in) { d_->in->stop(); d_->in->close(); d_->in.reset(); }
    if (d_->out) { d_->out->stop(); d_->out->close(); d_->out.reset(); }
    if (d_->enc) { opus_encoder_destroy(d_->enc); d_->enc = nullptr; }
    if (d_->dec) { opus_decoder_destroy(d_->dec); d_->dec = nullptr; }
}

} // namespace voicechat
