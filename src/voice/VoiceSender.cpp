#include "voice/VoiceSender.h"

#include <opus.h>

#include <algorithm>
#include <cmath>

namespace voicechat {

VoiceSender::VoiceSender(SenderConfig cfg, EmitFn emit) : cfg_(cfg), emit_(std::move(emit)) {
    cfg_.micGainPercent = std::clamp(cfg_.micGainPercent, 0, 800);
    cfg_.gateRms = std::clamp(cfg_.gateRms, 0, 8000);
    cfg_.hangoverFrames = std::clamp(cfg_.hangoverFrames, 0, 100);
    int err = 0;
    enc_ = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != 0 || !enc_) { enc_ = nullptr; return; }
    opus_encoder_ctl(enc_, OPUS_SET_BITRATE(std::clamp(cfg_.bitrate, 8000, 64000)));
    opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(5));
}

VoiceSender::~VoiceSender() {
    if (enc_) opus_encoder_destroy(enc_);
}

void VoiceSender::pushMic(const std::int16_t *pcm, int n) {
    if (!enc_ || n != kFrame || !pcm) return;

    std::int16_t buf[kFrame];
    double sumSq = 0;
    for (int i = 0; i < kFrame; i++) {
        const int v = std::clamp(pcm[i] * cfg_.micGainPercent / 100, -32768, 32767);
        buf[i] = static_cast<std::int16_t>(v);
        sumSq += static_cast<double>(v) * v;
    }
    const int rms = static_cast<int>(std::sqrt(sumSq / kFrame));
    rms_ = rms;

    if (cfg_.gateRms == 0 || rms >= cfg_.gateRms) hang_ = cfg_.hangoverFrames + 1;
    const bool open = hang_ > 0;
    if (hang_ > 0) hang_--;
    speaking_ = open;
    if (!open) return;

    unsigned char packet[400];
    const int len = opus_encode(enc_, buf, kFrame, packet, sizeof packet);
    if (len <= 0) return;
    sent_++;
    if (emit_) emit_(packet, len);
}

} // namespace voicechat
