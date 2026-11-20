#include "voice/Loopback.h"

#include "voice/RingBuffer.h"

#include <cstring> // Oboe 1.9's umbrella header uses memset
#include <android/log.h>
#include <oboe/Oboe.h>
#include <opus.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#define TAG "VoiceChat"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace voicechat {

static constexpr int kRate = 48000;
static constexpr int kFrame = 960;     // 20 ms @ 48 kHz, mono
static constexpr int kBitrate = 24000; // plenty for voice
static constexpr int kMaxPacket = 400;

static float dbToLinear(float db) {
    return std::pow(10.0f, db / 20.0f);
}

// Map config string -> Oboe preset. VoiceCommunication applies heavy AEC/NS and
// on several OEMs it suppresses speech while ambient noise still passes.
static oboe::InputPreset parsePreset(const std::string &name) {
    if (name == "voice_communication" || name == "communication")
        return oboe::InputPreset::VoiceCommunication;
    if (name == "generic")
        return oboe::InputPreset::Generic;
    if (name == "unprocessed")
        return oboe::InputPreset::Unprocessed;
    if (name == "camcorder")
        return oboe::InputPreset::Camcorder;
    if (name == "voice_performance" || name == "performance")
        return oboe::InputPreset::VoicePerformance;
    // Default / voice_recognition: speech-oriented without call AEC
    return oboe::InputPreset::VoiceRecognition;
}

static const char *presetName(oboe::InputPreset p) {
    switch (p) {
    case oboe::InputPreset::Generic:
        return "generic";
    case oboe::InputPreset::Camcorder:
        return "camcorder";
    case oboe::InputPreset::VoiceRecognition:
        return "voice_recognition";
    case oboe::InputPreset::VoiceCommunication:
        return "voice_communication";
    case oboe::InputPreset::Unprocessed:
        return "unprocessed";
    case oboe::InputPreset::VoicePerformance:
        return "voice_performance";
    default:
        return "unknown";
    }
}

struct Loopback::Impl : oboe::AudioStreamDataCallback {
    std::shared_ptr<oboe::AudioStream> in, out;
    OpusEncoder *enc = nullptr;
    OpusDecoder *dec = nullptr;
    SpscRing<int16_t> playback{kRate}; // ~1 s of decoded audio
    std::vector<int16_t> pending;      // mic samples waiting to fill a frame
    std::atomic<float> volume{1.0f};   // Self Hearing playback gain (linear)
    std::atomic<float> micGain{1.0f};  // Pre-encode mic gain (linear)
    oboe::InputPreset inputPreset = oboe::InputPreset::VoiceRecognition;

    Impl() { pending.reserve(kFrame * 8); }

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream *s, void *data, int32_t frames) override {
        if (s->getDirection() == oboe::Direction::Input) {
            auto *pcm = static_cast<const int16_t *>(data);
            const float g = micGain.load(std::memory_order_relaxed);

            // Apply mic gain into pending buffer (speech was getting lost under NS).
            pending.reserve(pending.size() + static_cast<size_t>(frames));
            for (int32_t i = 0; i < frames; ++i) {
                float sample = static_cast<float>(pcm[i]) * g;
                if (sample > 32767.f)
                    sample = 32767.f;
                if (sample < -32768.f)
                    sample = -32768.f;
                pending.push_back(static_cast<int16_t>(sample));
            }

            while (pending.size() >= static_cast<size_t>(kFrame)) {
                unsigned char packet[kMaxPacket];
                int n = opus_encode(enc, pending.data(), kFrame, packet, kMaxPacket);
                if (n > 0) {
                    // Standalone self-hearing test; multiplayer uses VoiceSession instead.
                    int16_t decoded[kFrame];
                    int got = opus_decode(dec, packet, n, decoded, kFrame, 0);
                    if (got > 0) {
                        const float pg = volume.load(std::memory_order_relaxed);
                        if (pg != 1.0f) {
                            for (int i = 0; i < got; ++i) {
                                float sample = static_cast<float>(decoded[i]) * pg;
                                if (sample > 32767.f)
                                    sample = 32767.f;
                                if (sample < -32768.f)
                                    sample = -32768.f;
                                decoded[i] = static_cast<int16_t>(sample);
                            }
                        }
                        playback.write(decoded, got);
                    }
                }
                pending.erase(pending.begin(), pending.begin() + kFrame);
            }
        } else {
            auto *pcm = static_cast<int16_t *>(data);
            size_t got = playback.read(pcm, frames);
            for (size_t i = got; i < static_cast<size_t>(frames); i++)
                pcm[i] = 0; // underrun -> silence
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
        if (dir == oboe::Direction::Input) {
            b.setInputPreset(inputPreset);
            // VoiceRecognition / Unprocessed: avoid call-style AEC that eats speech.
        }
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
    Options defaults;
    return start(defaults);
}

bool Loopback::start(const Options &options) {
    d_->inputPreset = parsePreset(options.inputPreset);
    d_->micGain.store(dbToLinear(options.micGainDb), std::memory_order_relaxed);
    d_->volume.store(options.playbackVolume < 0.f   ? 0.f
                     : options.playbackVolume > 2.f ? 2.f
                                                    : options.playbackVolume,
                     std::memory_order_relaxed);

    LOGI("Self Hearing capture: preset=%s micGain=%.1fdB playback=%.0f%%",
         presetName(d_->inputPreset), options.micGainDb, options.playbackVolume * 100.f);

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
    oboe::Result r = d_->out->requestStart();
    if (r == oboe::Result::OK) r = d_->in->requestStart();
    if (r != oboe::Result::OK) {
        LOGE("start audio failed: %s", oboe::convertToText(r));
        stop();
        return false;
    }
    return true;
}

void Loopback::stop() {
    if (d_->in) {
        d_->in->stop();
        d_->in->close();
        d_->in.reset();
    }
    if (d_->out) {
        d_->out->stop();
        d_->out->close();
        d_->out.reset();
    }
    if (d_->enc) {
        opus_encoder_destroy(d_->enc);
        d_->enc = nullptr;
    }
    if (d_->dec) {
        opus_decoder_destroy(d_->dec);
        d_->dec = nullptr;
    }
}

void Loopback::setVolume(float linear) {
    if (linear < 0.f)
        linear = 0.f;
    if (linear > 2.f)
        linear = 2.f;
    d_->volume.store(linear, std::memory_order_relaxed);
}

float Loopback::volume() const { return d_->volume.load(std::memory_order_relaxed); }

} // namespace voicechat
