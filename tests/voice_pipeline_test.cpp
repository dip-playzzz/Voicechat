// Mic -> VoiceSender -> (network) -> RemoteVoices -> speaker, using real libopus.
// Host build: g++ -Itests/host_opus -Isrc ... -l:libopus.so.0   (see tests/run_host_tests.sh)
#include "voice/RemoteVoices.h"
#include "voice/VoiceSender.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace voicechat;
#define CHECK(c, msg) do { if (!(c)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); std::exit(1); } } while (0)
constexpr double kPi = 3.14159265358979;

static std::vector<std::int16_t> sine(int frames, double hz, double amp, int startSample = 0) {
    std::vector<std::int16_t> v(frames * 320);
    for (size_t i = 0; i < v.size(); i++) v[i] = static_cast<std::int16_t>(amp * std::sin(2 * kPi * hz * (startSample + i) / 16000.0));
    return v;
}
using Packets = std::vector<std::vector<std::uint8_t>>;
static Packets encode(const std::vector<std::int16_t> &pcm, SenderConfig cfg = {}) {
    Packets out;
    VoiceSender s(cfg, [&](const std::uint8_t *p, int n) { out.emplace_back(p, p + n); });
    CHECK(s.ok(), "encoder created");
    for (size_t f = 0; f * 320 < pcm.size(); f++) s.pushMic(&pcm[f * 320], 320);
    return out;
}
static ForwardedVoice fwd(std::uint32_t id, std::uint16_t seq, const std::vector<std::uint8_t> &op, int dim, float x, float y, float z) {
    ForwardedVoice v; v.senderId = id; v.seq = seq; v.dim = dim; v.x = x; v.y = y; v.z = z; v.opus = op; return v;
}
struct Energy { double l = 0, r = 0; };
static Energy energy(const std::vector<std::int16_t> &st) {
    Energy e; for (size_t i = 0; i + 1 < st.size(); i += 2) { e.l += double(st[i]) * st[i]; e.r += double(st[i + 1]) * st[i + 1]; } return e;
}
static double goertzel(const std::vector<std::int16_t> &st, double hz) { // left channel power at hz
    double w = 2 * kPi * hz / 16000.0, c = 2 * std::cos(w), s1 = 0, s2 = 0;
    for (size_t i = 0; i < st.size(); i += 2) { double s = st[i] + c * s1 - s2; s2 = s1; s1 = s; }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}
// push all packets, then render `ticks` x 20 ms
static std::vector<std::int16_t> play(RemoteVoices &rv, int ticks) {
    std::vector<std::int16_t> out(ticks * 320 * 2);
    rv.render(out.data(), ticks * 320);
    return out;
}

int main() {
    // ---------- sender: gate, hangover, gain
    {
        auto loud = encode(sine(50, 440, 8000));
        CHECK(loud.size() == 50, "50 loud frames -> 50 packets");
        size_t bytes = 0; for (auto &p : loud) bytes += p.size();
        CHECK(bytes / loud.size() < 120, "voice packets are small (<120 B avg)");
        auto quiet = encode(sine(50, 440, 40));
        CHECK(quiet.empty(), "background noise below the gate is not sent");
        auto pcm = sine(5, 440, 8000); auto tail = sine(30, 440, 40); pcm.insert(pcm.end(), tail.begin(), tail.end());
        CHECK(encode(pcm).size() == 5 + 12, "5 loud frames + 12 hangover frames");
        SenderConfig g; g.micGainPercent = 800;
        CHECK(!encode(sine(20, 440, 100), g).empty(), "800% mic gain lifts a quiet mic over the gate");
        SenderConfig z; z.gateRms = 0;
        CHECK(encode(sine(10, 440, 5), z).size() == 10, "gate 0 = always send");
    }

    const Packets tone = encode(sine(50, 440, 8000));

    // ---------- listener at origin facing +Z (yaw 0); right-hand side is -X
    auto run = [&](int dim, float sx, float sz, float ldim = 0) {
        RemoteVoices rv({});
        rv.setListener(true, (int)ldim, 0, 64, 0, 0);
        for (size_t i = 0; i < tone.size(); i++) rv.onPacket(fwd(1, (std::uint16_t)i, tone[i], dim, sx, 64, sz));
        return play(rv, 60);
    };
    auto ahead = energy(run(0, 0, 10));
    CHECK(ahead.l > 1e8 && ahead.r > 1e8, "speaker 10 blocks ahead is audible");
    CHECK(std::fabs(ahead.l / ahead.r - 1.0) < 0.05, "straight ahead -> equal left/right");
    auto right = energy(run(0, -10, 0));
    CHECK(right.r > 6 * right.l, "speaker on the listener's right -> right channel louder");
    auto left = energy(run(0, 10, 0));
    CHECK(left.l > 6 * left.r, "speaker on the left -> left channel louder");
    auto far = energy(run(0, 0, 40));
    CHECK(far.l == 0 && far.r == 0, "beyond the radius -> silence");
    auto otherDim = energy(run(1, 0, 10));
    CHECK(otherDim.l == 0 && otherDim.r == 0, "other dimension -> silence");
    auto near5 = energy(run(0, 0, 5)), mid25 = energy(run(0, 0, 25));
    const double ratio = near5.l / mid25.l, want = std::pow((1 - 5 / 32.0) / (1 - 25 / 32.0), 4);
    CHECK(ratio > want * 0.7 && ratio < want * 1.3, "volume falls off with distance as designed");

    // ---------- the tone survives encode -> decode -> mix
    {
        auto out = run(0, 0, 8);
        CHECK(goertzel(out, 440) > 30 * goertzel(out, 1500), "440 Hz tone comes out as 440 Hz");
    }

    // ---------- no position known or player left the world: never play stale world audio
    {
        RemoteVoices rv({});
        rv.setListener(false, 0, 0, 0, 0, 0);
        for (size_t i = 0; i < tone.size(); i++) rv.onPacket(fwd(1, (std::uint16_t)i, tone[i], 0, 0, 64, 5));
        auto e = energy(play(rv, 60));
        CHECK(e.l == 0 && e.r == 0 && rv.speakerCount() == 0, "no listener position -> silence");
        rv.setListener(true, 0, 0, 64, 0, 0);
        for (size_t i = 0; i < tone.size(); i++) rv.onPacket(fwd(1, (std::uint16_t)i, tone[i], 0, 0, 64, 5));
        CHECK(energy(play(rv, 3)).l > 0, "audio plays with a valid listener position");
        rv.setListener(false, 0, 0, 0, 0, 0);
        e = energy(play(rv, 60));
        CHECK(e.l == 0 && e.r == 0 && rv.speakerCount() == 0, "leaving the world discards buffered audio");
    }

    // ---------- two nearby speakers can be mixed at the same time
    {
        auto other = encode(sine(30, 660, 5000));
        RemoteVoices rv({});
        rv.setListener(true, 0, 0, 64, 0, 0);
        for (int i = 0; i < 25; i++) {
            rv.onPacket(fwd(1, static_cast<std::uint16_t>(i), tone[i], 0, 0, 64, 5));
            rv.onPacket(fwd(2, static_cast<std::uint16_t>(i), other[i], 0, 0, 64, 5));
        }
        auto mixed = play(rv, 25);
        const double a = goertzel(mixed, 440), b = goertzel(mixed, 660), noise = goertzel(mixed, 1500);
        CHECK(a > 20 * noise && b > 20 * noise, "both simultaneous speakers survive mixing");
    }

    // ---------- render() chunking doesn't change the audio
    {
        auto build = [&] { auto rv = std::make_unique<RemoteVoices>(RemoteVoices::Config{});
            rv->setListener(true, 0, 0, 64, 0, 0);
            for (size_t i = 0; i < tone.size(); i++) rv->onPacket(fwd(1, (std::uint16_t)i, tone[i], 0, -4, 64, 6));
            return rv; };
        auto a = build(), b = build();
        std::vector<std::int16_t> big(3840 * 2), small; small.reserve(big.size());
        a->render(big.data(), 3840);
        for (int got = 0, n = 0; got < 3840; got += n) {
            static const int sizes[] = {192, 480, 1, 333, 100};
            n = std::min(sizes[(got + 7) % 5], 3840 - got);
            std::vector<std::int16_t> tmp(n * 2); b->render(tmp.data(), n);
            small.insert(small.end(), tmp.begin(), tmp.end());
        }
        CHECK(small == big, "chunked render == one big render");
    }

    // ---------- network trouble: reordering + 10% loss, packets trickling in tick by tick
    {
        RemoteVoices rv({});
        rv.setListener(true, 0, 0, 64, 0, 0);
        std::vector<std::int16_t> all;
        int played = 0;
        for (int t = 0; t < 60; t++) {
            // packet i "arrives" around tick i, but each odd/even pair is swapped; every 10th is lost
            for (int i : {t + 1 - (t % 2 == 0 ? 0 : 2), t}) { (void)i; }
            int a = (t % 2 == 0) ? t + 1 : t - 1;           // swapped partner
            if (a >= 0 && a < (int)tone.size() && a % 10 != 5) rv.onPacket(fwd(2, (std::uint16_t)a, tone[a], 0, 0, 64, 6));
            auto out = play(rv, 1);
            auto e = energy(out);
            if (t >= 5 && t < 45 && (e.l + e.r) > 1e5) played++;
        }
        CHECK(played >= 34, "reordered + lossy stream still plays >= 85% of ticks");
        CHECK(rv.concealed() >= 3, "lost frames were concealed (PLC)");
    }

    // ---------- sequence number wrap-around
    {
        RemoteVoices rv({});
        rv.setListener(true, 0, 0, 64, 0, 0);
        for (int i = 0; i < 40; i++) rv.onPacket(fwd(3, (std::uint16_t)(65516 + i), tone[i], 0, 0, 64, 5)); // crosses 65535 -> 0
        auto e = energy(play(rv, 45));
        CHECK(e.l > 1e8, "sequence wrap does not stall or drop audio");
        CHECK(rv.lateDropped() == 0, "nothing wrongly dropped as late");
    }

    // ---------- limits
    {
        RemoteVoices rv({});
        rv.setListener(true, 0, 0, 64, 0, 0);
        for (std::uint32_t id = 1; id <= 40; id++) rv.onPacket(fwd(id, 0, tone[0], 0, 0, 64, 5));
        CHECK(rv.speakerCount() <= 16, "speaker cap");
        ForwardedVoice junk = fwd(99, 0, {}, 0, 0, 64, 5); rv.onPacket(junk);          // empty
        junk.opus.assign(401, 1); rv.onPacket(junk);                                    // oversized
        junk.opus.assign(30, 0xff); for (int i = 0; i < 20; i++) { junk.seq = i; rv.onPacket(junk); }  // garbage opus
        play(rv, 30);                                                                    // must not crash
    }
    std::printf("voice pipeline tests ok\n");
}
