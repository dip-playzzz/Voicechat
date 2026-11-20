// Pretends to be the launcher's VoiceCaptureService (connects to the abstract socket and
// writes raw PCM in awkward chunk sizes) and checks the mod-side receiver. Linux host only.
#include "voice/MicSocketSource.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace voicechat;
using Clock = std::chrono::steady_clock;

static int connectTo(const std::string &name) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::memcpy(a.sun_path + 1, name.data(), name.size());
    socklen_t len = offsetof(sockaddr_un, sun_path) + 1 + name.size();
    if (::connect(fd, reinterpret_cast<sockaddr *>(&a), len) != 0) { ::close(fd); return -1; }
    return fd;
}
static void sendFrames(int fd, int firstIndex, int count, size_t chunk) {
    std::vector<int16_t> pcm;
    for (int f = 0; f < count; f++)
        for (int i = 0; i < 320; i++) pcm.push_back(static_cast<int16_t>((firstIndex + f) * 1000 + i));
    const char *p = reinterpret_cast<const char *>(pcm.data());
    size_t total = pcm.size() * 2, sent = 0;
    while (sent < total) {
        size_t n = std::min(chunk, total - sent);
        if (::send(fd, p + sent, n, 0) < 0) std::exit(2);
        sent += n;
    }
}
static bool waitFor(const MicSocketSource &s, uint64_t frames) {
    auto end = Clock::now() + std::chrono::seconds(3);
    while (s.framesReceived() < frames && Clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return s.framesReceived() >= frames;
}
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main() {
    const std::string name = "org.levimc.test.mic." + std::to_string(::getpid());
    std::mutex mu;
    std::vector<std::vector<int16_t>> got;
    MicSocketSource src(name, [&](const int16_t *s, int n) {
        std::lock_guard<std::mutex> l(mu);
        got.emplace_back(s, s + n);
    }, [](const std::string &m) { std::printf("  [src] %s\n", m.c_str()); });

    CHECK(src.start());

    // a second listener on the same name must fail cleanly
    MicSocketSource dup(name, nullptr, [](const std::string &m) { std::printf("  [dup] %s\n", m.c_str()); });
    CHECK(!dup.start());

    // 1) frames arrive intact even when the sender uses odd chunk sizes (reassembly)
    int c = connectTo(name);
    CHECK(c >= 0);
    sendFrames(c, 0, 5, 100);
    CHECK(waitFor(src, 5));
    CHECK(src.connected());
    {
        std::lock_guard<std::mutex> l(mu);
        CHECK(got.size() == 5);
        for (int f = 0; f < 5; f++)
            for (int i = 0; i < 320; i++) CHECK(got[f][i] == static_cast<int16_t>(f * 1000 + i));
    }

    // 2) launcher reconnects after a disconnect
    ::close(c);
    c = connectTo(name);
    CHECK(c >= 0);
    sendFrames(c, 5, 2, 641); // 641 = not a multiple of the frame size
    CHECK(waitFor(src, 7));

    // 3) stop() while a client is still connected returns promptly
    auto t0 = Clock::now();
    src.stop();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    CHECK(ms < 500);
    ::close(c);

    // 4) restart works and the name is free again
    CHECK(src.start());
    src.stop();
    std::printf("mic socket test ok (stop took %lld ms)\n", static_cast<long long>(ms));
}
