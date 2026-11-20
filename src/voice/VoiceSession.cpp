#include "voice/VoiceSession.h"

#include "voice/MicSocketSource.h"

#include <android/log.h>
#include <oboe/Oboe.h>

#define TAG "VoiceChat"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace voicechat {

struct VoiceSession::Impl : oboe::AudioStreamDataCallback {
    RemoteVoices remotes;
    std::unique_ptr<VoiceSender> sender;
    std::unique_ptr<MicSocketSource> mic;
    std::shared_ptr<oboe::AudioStream> out;
    std::atomic<std::uint16_t> seq{0};
    std::atomic<int> noPosition{0};

    explicit Impl(const RemoteVoices::Config &c) : remotes(c) {}

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream *, void *data, int32_t frames) override {
        remotes.render(static_cast<std::int16_t *>(data), frames); // stereo interleaved
        return oboe::DataCallbackResult::Continue;
    }
};

VoiceSession::VoiceSession(VoiceSessionConfig cfg, PositionFn position, LogFn log)
    : d_(new Impl(cfg.mixer)), cfg_(std::move(cfg)), position_(std::move(position)), log_(std::move(log)) {}

VoiceSession::~VoiceSession() { stop(); }

bool VoiceSession::start() {
    Impl *d = d_.get();

    d->sender = std::make_unique<VoiceSender>(cfg_.sender, [this, d](const std::uint8_t *opus, int n) {
        RelayClient *relay = relay_.load();
        if (!relay) return;
        const PlayerSnapshot s = position_ ? position_() : PlayerSnapshot{};
        if (!s.valid) {
            // Without a position the relay can't tell who is near whom, so say why we're silent (not too often).
            if (d->noPosition++ % 500 == 0 && log_)
                log_("Not sending voice: no player position yet (join a world; BedrockTools must be enabled "
                     "and Minecraft must be a supported version)");
            return;
        }
        relay->sendVoice(d->seq++, s.dimension, s.x, s.y, s.z, s.yaw, opus, n);
    });
    if (!d->sender->ok()) {
        LOGE("opus encoder failed to initialise");
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
    const oboe::Result r = b.openStream(d->out);
    if (r != oboe::Result::OK) {
        LOGE("open speaker stream failed: %s", oboe::convertToText(r));
        return false;
    }
    d->out->requestStart();

    VoiceSender *sender = d->sender.get();
    d->mic = std::make_unique<MicSocketSource>(
        cfg_.micSocketName, [sender](const std::int16_t *s, int n) { sender->pushMic(s, n); }, log_);
    if (!d->mic->start()) {
        stop();
        return false;
    }
    return true;
}

void VoiceSession::stop() {
    Impl *d = d_.get();
    if (d->mic) { d->mic->stop(); d->mic.reset(); } // joins the mic thread: no more sends after this
    if (d->out) { d->out->stop(); d->out->close(); d->out.reset(); }
    d->sender.reset();
}

void VoiceSession::onRemoteVoice(const ForwardedVoice &v) { d_->remotes.onPacket(v); }

void VoiceSession::setListener(const PlayerSnapshot &s) {
    d_->remotes.setListener(s.valid, s.dimension, s.x, s.y, s.z, s.yaw);
}

void VoiceSession::setMicGain(int percent) {
    cfg_.sender.micGainPercent = percent;
    if (d_->sender) d_->sender->setGain(percent);
}
void VoiceSession::setMicGate(int rms) {
    cfg_.sender.gateRms = rms;
    if (d_->sender) d_->sender->setGate(rms);
}
void VoiceSession::setRadius(float blocks) { d_->remotes.setRadius(blocks); }

std::string VoiceSession::status() const {
    const Impl *d = d_.get();
    std::string s = "mic ";
    s += (d->mic && d->mic->connected()) ? "connected" : "waiting for LeviLauncher's voice service";
    if (d->sender) {
        s += ", voice frames sent " + std::to_string(d->sender->framesSent());
        s += d->sender->speaking() ? " (speaking)" : "";
    }
    s += ", speakers heard now " + std::to_string(d->remotes.speakerCount());
    return s;
}

} // namespace voicechat
