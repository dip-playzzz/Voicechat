// Deterministic in-memory tests of the real WebRTC APM wrapper. These do not
// measure a phone's acoustic path, speech quality, or feedback safety.
#include "voice/WebRtcProcessor.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using voicechat::WebRtcProcessor;
#define CHECK(c, msg) do { if (!(c)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); std::exit(1); } } while (0)

static std::uint32_t state = 0x12345678u;
static std::int16_t randomSample(int amplitude) {
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return static_cast<std::int16_t>(static_cast<int>(state % (2 * amplitude + 1)) - amplitude);
}

int main() {
    CHECK(WebRtcProcessor::compiledIn(), "test must link the real APM");
    WebRtcProcessor disabled({});
    std::array<std::int16_t, WebRtcProcessor::kCaptureFrame> in{}, out{};
    std::array<std::int16_t, 2 * WebRtcProcessor::kBlock> stereo{};
    CHECK(!disabled.ready() && !disabled.processCapture(in.data(), out.data()) &&
          !disabled.processRender(stereo.data()), "disabled processor does not claim to process");

    // 40 ms synthetic echo of the exact, known stereo render reference. Allow
    // five seconds for adaptation, then measure echo-only energy. This is not
    // a device test, nor does it represent missing Minecraft game audio.
    WebRtcProcessor aec({true, false, 0});
    CHECK(aec.ready(), "create AEC3 at 16 kHz");
    CHECK(!aec.processCapture(nullptr, out.data()) && !aec.processCapture(in.data(), nullptr) &&
          !aec.processRender(nullptr), "invalid pointers are rejected");
    constexpr int frames = 750; // 15 s at 20 ms
    constexpr int delay = 640; // 40 ms at 16 kHz
    std::vector<std::int16_t> history(frames * WebRtcProcessor::kCaptureFrame);
    double before = 0, after = 0, nearSin = 0, nearCos = 0;
    std::size_t nearSamples = 0;
    constexpr double pi = 3.14159265358979323846;
    for (int f = 0; f < frames; ++f) {
        for (int i = 0; i < WebRtcProcessor::kCaptureFrame; ++i)
            history[f * WebRtcProcessor::kCaptureFrame + i] = randomSample(5000);
        for (int part = 0; part < 2; ++part) {
            for (int i = 0; i < WebRtcProcessor::kBlock; ++i) {
                auto s = history[f * WebRtcProcessor::kCaptureFrame + part * WebRtcProcessor::kBlock + i];
                stereo[2 * i] = s;
                stereo[2 * i + 1] = s;
            }
            CHECK(aec.processRender(stereo.data()), "process reverse 10 ms stereo");
        }
        for (int i = 0; i < WebRtcProcessor::kCaptureFrame; ++i) {
            const int index = f * WebRtcProcessor::kCaptureFrame + i;
            const int ix = index - delay;
            const int echo = ix >= 0 ? static_cast<int>(history[ix] * 0.55f) : 0;
            const int near = f >= 500 ? static_cast<int>(1800 * std::sin(2 * pi * 397 * index / 16000)) : 0;
            in[i] = static_cast<std::int16_t>(echo + near);
        }
        CHECK(aec.processCapture(in.data(), out.data()), "process 20 ms capture as two APM blocks");
        if (f >= 250 && f < 500) for (int i = 0; i < WebRtcProcessor::kCaptureFrame; ++i) {
            before += double(in[i]) * in[i];
            after += double(out[i]) * out[i];
        }
        if (f >= 550) for (int i = 0; i < WebRtcProcessor::kCaptureFrame; ++i) {
            const int index = f * WebRtcProcessor::kCaptureFrame + i;
            const double phase = 2 * pi * 397 * index / 16000;
            nearSin += out[i] * std::sin(phase);
            nearCos += out[i] * std::cos(phase);
            ++nearSamples;
        }
    }
    CHECK(before > 0 && after < before * 0.35, "matched synthetic echo is reduced after adaptation");
    const double toneAmplitude = 2 * std::hypot(nearSin, nearCos) / nearSamples;
    CHECK(toneAmplitude > 300, "independent synthetic near-end tone remains during double talk");
    std::printf("synthetic matched echo reduction: %.1f dB; double-talk tone amplitude: %.0f\n",
                10.0 * std::log10(before / after), toneAmplitude);

    // Sound absent from the reference cannot be reliably canceled. A silent
    // render stream and independent input demonstrate the essential limit.
    WebRtcProcessor unmatched({true, false, 0});
    CHECK(unmatched.ready(), "create unmatched-reference AEC");
    stereo.fill(0);
    before = after = 0;
    for (int f = 0; f < 200; ++f) {
        CHECK(unmatched.processRender(stereo.data()) && unmatched.processRender(stereo.data()), "silent reverse stream");
        for (auto &sample : in) sample = randomSample(3000);
        CHECK(unmatched.processCapture(in.data(), out.data()), "independent capture");
        if (f >= 50) for (int i = 0; i < WebRtcProcessor::kCaptureFrame; ++i) {
            before += double(in[i]) * in[i];
            after += double(out[i]) * out[i];
        }
    }
    CHECK(after > before * 0.1, "sound absent from the render reference is not reliably removed");

    WebRtcProcessor ns({false, true, 5000}); // out-of-range delay is safely bounded
    CHECK(ns.ready() && !ns.processRender(stereo.data()), "NS-only needs no reverse stream");
    before = after = 0;
    for (int f = 0; f < 500; ++f) {
        for (auto &sample : in) sample = randomSample(2500);
        CHECK(ns.processCapture(in.data(), out.data()), "noise suppression processes capture");
        if (f >= 250) for (int i = 0; i < WebRtcProcessor::kCaptureFrame; ++i) {
            before += double(in[i]) * in[i];
            after += double(out[i]) * out[i];
        }
    }
    CHECK(before > 0 && after < before * 0.8, "stationary synthetic noise is reduced");
    std::printf("synthetic noise reduction: %.1f dB\n", 10.0 * std::log10(before / after));
    std::puts("WEBRTC APM TESTS OK (synthetic signals only)");
}
