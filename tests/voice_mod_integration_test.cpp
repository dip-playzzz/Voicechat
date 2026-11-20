// Exercises the actual VoiceMod startup, tracker, relay and VoiceSession wiring on a PC.
// Oboe, Android permissions and BedrockTools events are stand-ins; Opus and UDP are real.
#include "mod/VoiceMod.h"
#include "voice/AndroidPermission.h"
#include "voice/VoiceSender.h"

#include <bedrocktools/Api.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <oboe/Oboe.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

using namespace voicechat;
using namespace std::chrono_literals;
namespace bt = bedrocktools;

#define CHECK(c, msg) do { if (!(c)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); std::exit(1); } } while (0)

// A real EventBus with the same copy-and-call semantics as BedrockTools.
bt::events::EventBus &bt::events::bus() { static EventBus b; return b; }
static std::uint64_t subscribe(bt::events::EventType type, bt::events::EventPriority priority,
                               bt::api::EventCallback cb, void *user) {
    return bt::events::bus().subscribeRaw(type, [type, cb, user](void *p) { cb(type, p, user); }, priority);
}
static void unsubscribe(std::uint64_t id) { bt::events::bus().unsubscribe(id); }
static const bt::api::ApiV1 runtime{bt::api::AbiVersion, sizeof(bt::api::ApiV1), nullptr, nullptr, subscribe, unsubscribe};
extern "C" const bt::api::ApiV1 *BedrockTools_GetApi(std::uint32_t version) {
    return version == bt::api::AbiVersion ? &runtime : nullptr;
}

static pl::mod::NativeMod *currentMod = nullptr;
pl::mod::NativeMod *pl::mod::NativeMod::current() noexcept { return currentMod; }
namespace voicechat {
MicStatus queryMicPermission() { return {true, "host-test", true, true, {}}; }
std::string requestMicPermission() { return "host test"; }
}

static bool waitFor(auto pred, std::chrono::milliseconds timeout) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return pred();
}

static int connectLauncherMic() {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(fd >= 0, "create launcher socket");
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const char *name = "org.levimc.voicechatlevi.audio";
    std::memcpy(addr.sun_path + 1, name, std::strlen(name));
    const auto len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + std::strlen(name));
    CHECK(::connect(fd, reinterpret_cast<sockaddr *>(&addr), len) == 0, "launcher connects to mod's socket");
    return fd;
}

static void sendAll(int fd, const void *data, std::size_t length) {
    const auto *p = static_cast<const char *>(data);
    while (length) {
        const auto sent = ::send(fd, p, length, MSG_NOSIGNAL);
        CHECK(sent > 0, "write launcher PCM");
        p += sent;
        length -= static_cast<std::size_t>(sent);
    }
}

static void makeTone(std::int16_t *frame, int frameIndex, double frequency) {
    constexpr double pi = 3.141592653589793;
    for (int i = 0; i < 320; ++i)
        frame[i] = static_cast<std::int16_t>(8000 * std::sin(2 * pi * frequency * (frameIndex * 320 + i) / 16000));
}

static double energy(const std::vector<std::int16_t> &samples) {
    double e = 0;
    for (auto s : samples) e += double(s) * s;
    return e;
}

static void writeConfig(const std::filesystem::path &dir, int port, const char *source) {
    std::filesystem::create_directories(dir / "config");
    std::ofstream f(dir / "config" / "config.json");
    f << "{\"enabled\":true,\"useRelay\":true,\"serverHost\":\"127.0.0.1\",\"serverPort\":" << port
      << ",\"room\":\"test\",\"secret\":\"secret\",\"playerId\":100,\"micSource\":\"" << source
      << "\",\"selfHearing\":false,\"logPosition\":false,\"micGate\":0}";
    CHECK(bool(f), "write mod config");
}

int main(int argc, char **argv) {
    CHECK(argc == 3, "usage: voice_mod_integration_test <port> <temporary directory>");
    const int port = std::atoi(argv[1]);
    const auto root = std::filesystem::path(argv[2]) / "Minecraft_1.26.60.4" / "mods" / "voicechat";
    pl::mod::ModInfo info;
    info.id = "voicechat"; info.displayName = "Voice test"; info.modRootPath = root;
    pl::mod::NativeMod native(nullptr, info);
    currentMod = &native;

    PlayerSnapshot player;
    player.valid = true; player.x = 0; player.y = 64; player.z = 0; player.dimension = 0;
    bt::events::LocalPlayerTickEvent ev{reinterpret_cast<bt::sdk::Player *>(&player)};
    std::atomic<bool> ticking{true}, run{true};
    std::thread ticks([&] {
        while (run) {
            if (ticking) bt::events::bus().publish(ev);
            std::this_thread::sleep_for(20ms);
        }
    });

    std::atomic<int> received{0};
    RelayClient peer({"127.0.0.1", static_cast<std::uint16_t>(port), "test", "secret", 200}, nullptr,
                     [&](const ForwardedVoice &) { received++; });
    peer.start();
    CHECK(waitFor([&] { return peer.connected(); }, 5s), "peer connects to relay");
    std::thread peerPosition([&] {
        while (run) { peer.sendPosition(0, 4, 64, 0, 0); std::this_thread::sleep_for(100ms); }
    });

    auto &mod = VoiceMod::instance();
    for (const char *source : {"direct", "launcher"}) {
        writeConfig(root, port, source);
        CHECK(mod.load() && mod.enable(), "load and enable real VoiceMod");
        CHECK(waitFor([&] { return !oboe::test::running(oboe::Direction::Output).empty(); }, 5s), "output started");
        std::shared_ptr<oboe::AudioStream> input;
        int socketFd = -1;
        if (std::strcmp(source, "direct") == 0) {
            CHECK(waitFor([&] { return !oboe::test::running(oboe::Direction::Input).empty(); }, 5s), "direct input started");
            input = oboe::test::running(oboe::Direction::Input).back();
        } else {
            socketFd = connectLauncherMic();
        }
        auto output = oboe::test::running(oboe::Direction::Output).back();
        std::this_thread::sleep_for(800ms); // tracker tick, relay HELLO and position pump

        const int before = received.load();
        for (int f = 0; f < 50; ++f) {
            std::int16_t frame[320]; makeTone(frame, f, 440);
            if (input) {
                // Real Oboe callbacks are not guaranteed to arrive in 20 ms chunks.
                input->testCallback(frame, 113);
                input->testCallback(frame + 113, 207);
            } else {
                sendAll(socketFd, frame, 211);
                sendAll(socketFd, reinterpret_cast<const char *>(frame) + 211, sizeof(frame) - 211);
            }
            std::this_thread::sleep_for(20ms);
        }
        CHECK(waitFor([&] { return received.load() >= before + 30; }, 3s), "mic reaches another player through actual mod startup");
        std::vector<std::int16_t> silent(320 * 2);
        output->testCallback(silent.data(), 320);
        CHECK(energy(silent) == 0, "self-hearing off does not play own mic");

        // Peer -> actual relay callback -> VoiceSession -> output callback.
        std::uint16_t seq = 0;
        VoiceSender other({}, [&](const std::uint8_t *p, int n) { peer.sendVoice(seq++, 0, 4, 64, 0, 0, p, n); });
        CHECK(other.ok(), "peer Opus encoder");
        for (int f = 0; f < 35; ++f) {
            std::int16_t frame[320]; makeTone(frame, f, 660);
            other.pushMic(frame, 320);
            std::this_thread::sleep_for(20ms);
        }
        std::this_thread::sleep_for(150ms);
        std::vector<std::int16_t> audible(30 * 320 * 2);
        output->testCallback(audible.data(), 30 * 320);
        CHECK(energy(audible) > 1e8, "remote player's audio reaches the real mod output");

        if (input) {
            ticking = false; // like leaving a world
            std::this_thread::sleep_for(2600ms);
            const int stale = received.load();
            for (int f = 0; f < 20; ++f) {
                std::int16_t frame[320]; makeTone(frame, f, 440);
                input->testCallback(frame, 320);
                std::this_thread::sleep_for(20ms);
            }
            std::this_thread::sleep_for(200ms);
            CHECK(received.load() == stale, "voice stops when player ticks go stale");
            ticking = true;
        }
        if (socketFd >= 0) ::close(socketFd);
        CHECK(mod.disable() && mod.unload(), "stop and unload real VoiceMod");
        input.reset(); output.reset();
        std::printf("voice mod %s microphone integration ok\n", source);
    }

    // An older config without selfHearing must preserve its loopbackTest=false choice.
    {
        std::ofstream old(root / "config" / "config.json");
        old << "{\"enabled\":true,\"useRelay\":false,\"micSource\":\"launcher\",\"loopbackTest\":false}";
    }
    CHECK(mod.load() && mod.enable(), "load legacy configuration");
    std::this_thread::sleep_for(100ms);
    CHECK(oboe::test::running(oboe::Direction::Output).empty(), "legacy self-hearing off remains off");
    {
        std::ifstream f(root / "config" / "config.json");
        nlohmann::json migrated; f >> migrated;
        CHECK(migrated["selfHearing"] == false, "legacy self-hearing choice is persisted");
    }
    CHECK(mod.disable() && mod.unload(), "stop legacy configuration");

    // Self-hearing shares the same direct microphone and is cleared on restart.
    {
        VoiceSessionConfig cfg;
        cfg.micSource = "direct"; cfg.selfHearing = true; cfg.selfHearingVolume = 0.5f;
        VoiceSession session(cfg, [] { return PlayerSnapshot{}; }, nullptr);
        CHECK(session.start() && session.startDirectMic(), "start self-hearing session");
        auto in = oboe::test::running(oboe::Direction::Input).back();
        auto out = oboe::test::running(oboe::Direction::Output).back();
        std::int16_t samples[320];
        for (auto &s : samples) s = 12000;
        in->testCallback(samples, 320);
        std::vector<std::int16_t> played(320 * 2);
        CHECK(waitFor([&] { out->testCallback(played.data(), 320); return energy(played) > 1e8; }, 2s),
              "self-hearing plays the microphone");
        CHECK(played[0] == 6000 && played[1] == 6000, "self-hearing respects volume and plays in both channels");
        session.stop();
        CHECK(session.start(), "restart self-hearing session");
        out = oboe::test::running(oboe::Direction::Output).back();
        std::vector<std::int16_t> afterRestart(320 * 2);
        out->testCallback(afterRestart.data(), 320);
        CHECK(energy(afterRestart) == 0, "restart does not replay old microphone audio");
        session.stop();
    }

    run = false;
    ticks.join(); peerPosition.join(); peer.stop();
    std::puts("VOICE MOD INTEGRATION OK");
}
