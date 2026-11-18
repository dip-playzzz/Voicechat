#include "voice/RemoteVoices.h"

#include "voice/Proximity.h"

#include <opus.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace voicechat {

RemoteVoices::RemoteVoices(Config cfg) : cfg_(cfg) {
    cfg_.jitterFrames = std::clamp(cfg_.jitterFrames, 1, 10);
    cfg_.maxQueue = std::clamp(cfg_.maxQueue, 5, 100);
    std::memset(fifo_, 0, sizeof fifo_);
}

RemoteVoices::~RemoteVoices() {
    for (auto &[id, sp] : speakers_) if (sp.dec) opus_decoder_destroy(sp.dec);
}

int RemoteVoices::speakerCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return static_cast<int>(speakers_.size());
}

void RemoteVoices::setListener(bool valid, int dim, float x, float y, float z, float yawDeg) {
    std::lock_guard<std::mutex> lock(mu_);
    lValid_ = valid; lDim_ = dim; lx_ = x; ly_ = y; lz_ = z; lyaw_ = yawDeg;
}

void RemoteVoices::onPacket(const ForwardedVoice &v) {
    if (v.opus.empty() || v.opus.size() > 400) return;
    std::lock_guard<std::mutex> lock(mu_);
    auto it = speakers_.find(v.senderId);
    if (it == speakers_.end()) {
        if (static_cast<int>(speakers_.size()) >= cfg_.maxSpeakers) return;
        int err = 0;
        OpusDecoder *dec = opus_decoder_create(16000, 1, &err);
        if (err != 0 || !dec) return;
        it = speakers_.emplace(v.senderId, Speaker{}).first;
        it->second.dec = dec;
        it->second.lastExt = v.seq;
    }
    Speaker &sp = it->second;

    // unwrap the 16-bit sequence number into a monotonically increasing 64-bit one
    const auto diff = static_cast<std::int16_t>(static_cast<std::uint16_t>(v.seq - static_cast<std::uint16_t>(sp.lastExt)));
    const std::int64_t ext = sp.lastExt + diff;
    if (ext > sp.lastExt) sp.lastExt = ext;

    if (sp.playing && ext < sp.next) { late_++; return; } // arrived after its slot was played
    sp.q[ext] = v.opus;
    while (static_cast<int>(sp.q.size()) > cfg_.maxQueue) {
        sp.q.erase(sp.q.begin());
        if (sp.playing) sp.next = std::max(sp.next, sp.q.begin()->first);
    }
    sp.dim = v.dim; sp.x = v.x; sp.y = v.y; sp.z = v.z;
    sp.seen = std::chrono::steady_clock::now();
}

void RemoteVoices::tick(std::int16_t *out) {
    std::int32_t acc[kFrame * 2] = {};
    const auto now = std::chrono::steady_clock::now();

    for (auto it = speakers_.begin(); it != speakers_.end();) {
        Speaker &sp = it->second;

        if (!sp.playing && !sp.q.empty()) {
            sp.waited++;
            if (static_cast<int>(sp.q.size()) >= cfg_.jitterFrames || sp.waited > cfg_.jitterFrames) {
                sp.playing = true;
                sp.next = sp.q.begin()->first;
                sp.idle = 0;
            }
        }

        std::int16_t pcm[kFrame];
        bool have = false;
        if (sp.playing) {
            auto f = sp.q.find(sp.next);
            if (f != sp.q.end()) {
                have = opus_decode(sp.dec, f->second.data(), static_cast<opus_int32>(f->second.size()), pcm, kFrame, 0) == kFrame;
                sp.q.erase(f);
                sp.next++;
                sp.idle = 0;
            } else if (!sp.q.empty()) {
                if (sp.q.begin()->first - sp.next > 10) {
                    sp.next = sp.q.begin()->first; // big hole: resynchronise instead of inventing speech
                } else {
                    have = opus_decode(sp.dec, nullptr, 0, pcm, kFrame, 0) == kFrame; // conceal one lost frame
                    sp.next++;
                    plc_++;
                }
            } else if (++sp.idle >= 3) {
                sp.playing = false; // speaker went quiet; re-buffer for their next sentence
                sp.waited = 0;
            }
        }

        if (have && (!lValid_ || sp.dim == lDim_)) {
            float gain = 1.f, pan = 0.f;
            if (lValid_) {
                const float dx = sp.x - lx_, dy = sp.y - ly_, dz = sp.z - lz_;
                gain = proximityGain(std::sqrt(dx * dx + dy * dy + dz * dz), cfg_.radius);
                pan = stereoPan(lx_, lz_, lyaw_, sp.x, sp.z);
            }
            if (gain > 0.f) {
                const float angle = (pan + 1.f) * 0.7853982f; // constant-power pan: centre = 1.0 on both sides
                const float gl = gain * std::cos(angle) * 1.4142135f, gr = gain * std::sin(angle) * 1.4142135f;
                for (int i = 0; i < kFrame; i++) {
                    acc[2 * i] += static_cast<std::int32_t>(pcm[i] * gl);
                    acc[2 * i + 1] += static_cast<std::int32_t>(pcm[i] * gr);
                }
            }
        }

        if (sp.q.empty() && !sp.playing && now - sp.seen > std::chrono::seconds(3)) {
            opus_decoder_destroy(sp.dec);
            it = speakers_.erase(it);
        } else {
            ++it;
        }
    }
    for (int i = 0; i < kFrame * 2; i++) out[i] = static_cast<std::int16_t>(std::clamp(acc[i], -32768, 32767));
}

void RemoteVoices::render(std::int16_t *stereo, int frames) {
    if (frames <= 0) return;
    std::lock_guard<std::mutex> lock(mu_);
    const int need = frames * 2;
    int done = 0;
    while (done < need) {
        if (fifoCount_ == 0) { tick(fifo_); fifoCount_ = kFrame * 2; }
        const int take = std::min(need - done, fifoCount_);
        std::memcpy(stereo + done, fifo_, take * sizeof(std::int16_t));
        std::memmove(fifo_, fifo_ + take, (fifoCount_ - take) * sizeof(std::int16_t));
        fifoCount_ -= take;
        done += take;
    }
}

} // namespace voicechat
