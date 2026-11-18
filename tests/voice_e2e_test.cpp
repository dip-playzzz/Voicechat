// Whole voice path over real UDP: A's "microphone" -> VoiceSender -> RelayClient -> relay/server.py
// -> RelayClient -> RemoteVoices -> stereo samples.  usage: voice_e2e_test <port> <secret>
#include "voice/RelayClient.h"
#include "voice/RemoteVoices.h"
#include "voice/VoiceSender.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace voicechat;
using namespace std::chrono_literals;
#define CHECK(c, msg) do { if (!(c)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); std::fflush(stdout); std::exit(1); } } while (0)

struct Listener {
    RemoteVoices mix{{}};
    RelayClient client;
    Listener(std::uint32_t id, int port, const std::string &secret)
        : client({"127.0.0.1", static_cast<std::uint16_t>(port), "room", secret, id}, nullptr,
                 [this](const ForwardedVoice &v) { mix.onPacket(v); }) {}
};
static double goertzel(const std::int16_t *st, size_t n, double hz) {
    double w = 2 * 3.14159265358979 * hz / 16000.0, c = 2 * std::cos(w), s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; i += 2) { double s = st[i] + st[i + 1] + c * s1 - s2; s2 = s1; s1 = s; }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

int main(int argc, char **argv) {
    CHECK(argc >= 3, "usage");
    const int port = std::atoi(argv[1]);
    const std::string secret = argv[2];

    RelayClient A({"127.0.0.1", static_cast<std::uint16_t>(port), "room", secret, 1}, nullptr, nullptr);
    Listener B(2, port, secret), C(3, port, secret);
    A.start(); B.client.start(); C.client.start();
    for (int i = 0; i < 100 && !(A.connected() && B.client.connected() && C.client.connected()); i++) std::this_thread::sleep_for(50ms);
    CHECK(A.connected() && B.client.connected() && C.client.connected(), "all three connect");

    // A at origin; B 8 blocks away on A's... B faces +Z at origin+(8,0); C is 100 blocks away
    B.mix.setListener(true, 0, 8, 64, 0, 0);  C.mix.setListener(true, 0, 100, 64, 0, 0);
    auto place = [&] { A.sendPosition(0, 0, 64, 0, 0); B.client.sendPosition(0, 8, 64, 0, 0); C.client.sendPosition(0, 100, 64, 0, 0); };
    place(); std::this_thread::sleep_for(300ms);

    // A speaks 1.5 s of a 440 Hz tone through the real encoder, paced like a live mic (20 ms per frame)
    std::uint16_t seq = 0;
    VoiceSender mic({}, [&](const std::uint8_t *p, int n) { A.sendVoice(seq++, 0, 0, 64, 0, 0, p, n); });
    std::int16_t frame[320];
    for (int f = 0; f < 75; f++) {
        for (int i = 0; i < 320; i++) frame[i] = static_cast<std::int16_t>(8000 * std::sin(2 * 3.14159265358979 * 440 * (f * 320 + i) / 16000.0));
        mic.pushMic(frame, 320);
        if (f % 5 == 0) place();
        std::this_thread::sleep_for(20ms);
    }
    std::this_thread::sleep_for(300ms);

    CHECK(B.client.voiceReceived() >= 70, "B received nearly all packets over real UDP");
    CHECK(C.client.voiceReceived() == 0, "C (100 blocks) received nothing from the server");

    std::vector<std::int16_t> outB(100 * 320 * 2), outC(100 * 320 * 2);
    B.mix.render(outB.data(), 100 * 320);
    C.mix.render(outC.data(), 100 * 320);
    const double tone = goertzel(outB.data(), outB.size(), 440), other = goertzel(outB.data(), outB.size(), 1500);
    CHECK(tone > 1e9 && tone > 30 * other, "B hears A's 440 Hz tone");
    double eC = 0; for (auto s : outC) eC += double(s) * s;
    CHECK(eC == 0, "C hears silence");
    std::printf("VOICE E2E OK (B got %llu packets)\n", (unsigned long long)B.client.voiceReceived());
    A.stop(); B.client.stop(); C.client.stop();
}
