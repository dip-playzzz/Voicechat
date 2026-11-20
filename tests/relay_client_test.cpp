// Drives several RelayClients against a real relay/server.py. Run via tests/relay_integration.py.
// usage: relay_client_test <port> <secret>
#include "voice/RelayClient.h"
#include "voice/Sha256.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace voicechat;
using namespace std::chrono_literals;

static std::string hex(const std::uint8_t *p, size_t n) {
    static const char *h = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) { s += h[p[i] >> 4]; s += h[p[i] & 15]; }
    return s;
}
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); std::fflush(stdout); std::exit(1); } } while (0)

struct Peer {
    RelayClient client;
    std::mutex mu;
    std::vector<ForwardedVoice> heard;
    Peer(std::uint32_t id, int port, const std::string &secret, const char *name, const char *host = "127.0.0.1")
        : client({host, static_cast<std::uint16_t>(port), "room1", secret, id},
                 [name](const std::string &m) { std::printf("  [%s] %s\n", name, m.c_str()); },
                 [this](const ForwardedVoice &v) { std::lock_guard<std::mutex> l(mu); heard.push_back(v); }) {}
    size_t count() { std::lock_guard<std::mutex> l(mu); return heard.size(); }
    void clear() { std::lock_guard<std::mutex> l(mu); heard.clear(); }
};

static bool waitFor(auto pred, std::chrono::milliseconds t) {
    const auto end = std::chrono::steady_clock::now() + t;
    while (std::chrono::steady_clock::now() < end) { if (pred()) return true; std::this_thread::sleep_for(20ms); }
    return pred();
}

int main(int argc, char **argv) {
    CHECK(argc >= 3, "usage: relay_client_test <port> <secret>");
    const int port = std::atoi(argv[1]);
    const std::string secret = argv[2];

    // --- crypto known-answer tests
    { Sha256 s; s.update("abc", 3); auto d = s.finish();
      CHECK(hex(d.data(), 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256(abc)"); }
    { auto d = hmacSha256("key", "The quick brown fox jumps over the lazy dog");
      CHECK(hex(d.data(), 32) == "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8", "hmac-sha256"); }
    { std::string longKey(100, 'k'); auto d = hmacSha256(longKey, "x"); CHECK(d.size() == 32, "hmac long key"); }

    // localhost commonly resolves to IPv6 first, while the bundled relay listens on IPv4.
    Peer A(1, port, secret, "A", "localhost"), B(2, port, secret, "B"), C(3, port, secret, "C"), D(4, port, "WRONG", "D");
    A.client.start(); B.client.start(); C.client.start(); D.client.start();

    CHECK(waitFor([&] { return A.client.connected() && B.client.connected() && C.client.connected(); }, 5s),
          "A, B and C should connect with the right secret");
    std::this_thread::sleep_for(1500ms);
    CHECK(!D.client.connected(), "D (wrong secret) must NOT connect");

    // positions: A at origin, B 10 blocks away, C 100 blocks away
    auto place = [&] {
        A.client.sendPosition(0, 0, 64, 0, 0);
        B.client.sendPosition(0, 10, 64, 0, 0);
        C.client.sendPosition(0, 100, 64, 0, 0);
    };
    place();
    std::this_thread::sleep_for(300ms);

    const std::uint8_t opus[5] = {9, 8, 7, 6, 5};
    CHECK(A.client.sendVoice(42, 0, 0, 64, 0, 0, opus, sizeof opus), "A sendVoice");
    CHECK(waitFor([&] { return B.count() >= 1; }, 2s), "B (10 blocks) should hear A");
    std::this_thread::sleep_for(300ms);
    {
        std::lock_guard<std::mutex> l(B.mu);
        CHECK(B.heard.size() == 1, "B hears exactly one packet");
        const auto &v = B.heard[0];
        CHECK(v.senderId == 1 && v.seq == 42 && v.dim == 0 && v.opus.size() == 5 && v.opus[0] == 9 && v.opus[4] == 5,
              "forwarded fields intact");
    }
    CHECK(C.count() == 0, "C (100 blocks) must not hear A");
    CHECK(A.count() == 0, "A must not hear itself");

    // different dimension -> silent
    B.clear();
    B.client.sendPosition(1, 10, 64, 0, 0);
    std::this_thread::sleep_for(300ms);
    A.client.sendVoice(43, 0, 0, 64, 0, 0, opus, sizeof opus);
    std::this_thread::sleep_for(500ms);
    CHECK(B.count() == 0, "B in another dimension must not hear A");
    B.client.sendPosition(0, 10, 64, 0, 0);
    std::this_thread::sleep_for(300ms);

    std::printf("PHASE1_OK\n"); std::fflush(stdout);

    // phase 2: harness restarts the server; clients must reconnect on their own
    std::string line;
    std::getline(std::cin, line);
    CHECK(line == "RESTARTED", "harness handshake");
    const auto c0 = A.client.connectCount();
    CHECK(waitFor([&] { return A.client.connectCount() > c0 && B.client.connectCount() > 1; }, 20s),
          "A and B should reconnect after the server restarts");
    place();
    std::this_thread::sleep_for(400ms);
    B.clear();
    A.client.sendVoice(44, 0, 0, 64, 0, 0, opus, sizeof opus);
    CHECK(waitFor([&] { return B.count() >= 1; }, 2s), "voice flows again after reconnect");

    std::printf("PHASE2_OK\n"); std::fflush(stdout);
    A.client.stop(); B.client.stop(); C.client.stop(); D.client.stop();
    return 0;
}
