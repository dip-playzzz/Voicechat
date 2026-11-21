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
#include <limits>
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

static void writeConfig(const std::filesystem::path &dir, int port, const char *source, bool processing) {
    std::filesystem::create_directories(dir / "config");
    std::ofstream f(dir / "config" / "config.json");
    // Emulate an upgrade from the old unsafe defaults while multiplayer is on.
    f << "{\"version\":3,\"enabled\":true,\"useRelay\":true,\"serverHost\":\"127.0.0.1\",\"serverPort\":" << port
      << ",\"room\":\"test\",\"secret\":\"secret\",\"playerId\":100,\"micSource\":\"" << source
      << "\",\"selfHearing\":true,\"selfHearingVolume\":1.0,\"micGainDb\":6,\"logPosition\":false,\"micGate\":0";
    if (processing) f << ",\"webrtcVoiceAec\":true,\"webrtcNoiseSuppression\":true";
    f << "}";
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
    struct Case { const char *source; bool processing; };
    for (const auto test : {Case{"direct", false}, Case{"launcher", false},
                            Case{"direct", true}, Case{"launcher", true}}) {
        const char *source = test.source;
        writeConfig(root, port, source, test.processing);
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
        CHECK(energy(silent) == 0, "old self-hearing default is disabled while the mic still sends");

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
        for (int f = 0; f < 30; ++f) {
            output->testCallback(audible.data() + f * 320 * 2, 320);
            std::this_thread::sleep_for(5ms); // let the reference worker keep up
        }
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
        std::printf("voice mod %s microphone integration ok (processing requested: %s)\n",
                    source, test.processing ? "yes" : "no");
    }

    const auto configPath = root / "config" / "config.json";
    auto readConfig = [&] {
        std::ifstream f(configPath);
        nlohmann::json result;
        f >> result;
        return result;
    };
    auto noRunningAudio = [] {
        return oboe::test::running(oboe::Direction::Input).empty() &&
               oboe::test::running(oboe::Direction::Output).empty();
    };

    // A fresh install never starts microphone monitoring automatically.
    CHECK(std::filesystem::remove(configPath), "remove old test config");
    CHECK(mod.load() && mod.enable(), "load fresh default config");
    std::this_thread::sleep_for(100ms);
    CHECK(noRunningAudio(), "fresh defaults do not start self-hearing");
    {
        const auto cfg = readConfig();
        CHECK(cfg["version"] == kConfigVersion && cfg["selfHearing"] == false,
              "fresh config records monitoring off");
        CHECK(std::abs(cfg["selfHearingVolume"].get<float>() - 0.1f) < 0.0001f &&
              cfg["micGainDb"] == 0.0 && cfg["loopbackTest"] == false,
              "fresh monitoring gains have safer defaults");
        CHECK(cfg["webrtcVoiceAec"] == false && cfg["webrtcNoiseSuppression"] == false &&
              cfg["webrtcAecDelayMs"] == 50, "experimental processing is off by default");
    }
    CHECK(mod.disable() && mod.unload(), "stop fresh configuration");

    // The previously published v3 config could start loud feedback immediately.
    // Migration is performed before the config version is rewritten, preserves
    // non-monitoring settings, and does not start either audio stream.
    {
        std::ofstream old(configPath);
        old << R"({"version":3,"enabled":true,"useRelay":false,"micSource":"direct",)"
            << R"("selfHearing":true,"selfHearingVolume":1.5,"micGainDb":6,"loopbackTest":true,)"
            << R"("serverHost":"keep.example","room":"keep-room","secret":"keep-secret","logPosition":false})";
    }
    CHECK(mod.load() && mod.enable(), "upgrade old v3 configuration");
    std::this_thread::sleep_for(100ms);
    CHECK(noRunningAudio(), "v3 upgrade cannot start self-hearing");
    {
        const auto cfg = readConfig();
        CHECK(cfg["version"] == kConfigVersion && cfg["selfHearing"] == false && cfg["loopbackTest"] == false,
              "v3 monitoring disabled and migrated");
        CHECK(std::abs(cfg["selfHearingVolume"].get<float>() - 0.1f) < 0.0001f && cfg["micGainDb"] == 0.0,
              "v3 dangerous monitoring gains reset");
        CHECK(cfg["serverHost"] == "keep.example" && cfg["room"] == "keep-room" && cfg["secret"] == "keep-secret",
              "v3 relay settings preserved");
    }
    CHECK(mod.disable() && mod.unload(), "stop v3 configuration");

    // A still older loopbackTest=true must not silently reactivate monitoring.
    {
        std::ofstream old(configPath);
        old << R"({"enabled":true,"useRelay":false,"micSource":"launcher","loopbackTest":true})";
    }
    CHECK(mod.load() && mod.enable(), "load legacy configuration");
    std::this_thread::sleep_for(100ms);
    CHECK(noRunningAudio(), "legacy loopback choice does not auto-start monitoring");
    CHECK(readConfig()["selfHearing"] == false && readConfig()["loopbackTest"] == false,
          "legacy settings safely migrated");
    CHECK(mod.disable() && mod.unload(), "stop legacy configuration");

    // Force a real migration write failure without relying on chmod or root:
    // ConfigFile writes to config.json.tmp, so a directory at that path makes
    // opening the temporary file fail. The old unsafe config must stay intact
    // and audio must not start; after removing the obstruction it can migrate.
    {
        std::ofstream old(configPath);
        old << R"({"version":3,"enabled":true,"useRelay":false,"micSource":"direct",)"
            << R"("selfHearing":true,"selfHearingVolume":2.0,"micGainDb":6})";
    }
    const auto blockedTempPath = std::filesystem::path(configPath.string() + ".tmp");
    CHECK(std::filesystem::create_directory(blockedTempPath), "block migration's temporary file");
    CHECK(!mod.load() && !mod.enable(), "failed migration write prevents enabling");
    CHECK(noRunningAudio() && readConfig()["version"] == 3 && readConfig()["selfHearing"] == true,
          "failed migration does not start audio or replace the old config");
    CHECK(mod.unload(), "unload after migration write failure");
    CHECK(std::filesystem::remove(blockedTempPath), "unblock migration's temporary file");
    CHECK(mod.load() && mod.enable(), "retry migration after write is possible");
    CHECK(noRunningAudio() && readConfig()["version"] == kConfigVersion && readConfig()["selfHearing"] == false,
          "successful retry disables the old unsafe monitoring settings");
    CHECK(mod.disable() && mod.unload(), "stop after migration retry");

    // A deliberate v4 opt-in is respected rather than reset on every launch.
    {
        std::ofstream chosen(configPath);
        chosen << R"({"version":4,"enabled":true,"useRelay":false,"micSource":"direct",)"
               << R"("selfHearing":true,"selfHearingVolume":0.05,"micGainDb":-3,"logPosition":false})";
    }
    CHECK(mod.load() && mod.enable(), "load explicit v4 self-hearing opt-in");
    CHECK(waitFor([&] { return !oboe::test::running(oboe::Direction::Input).empty() &&
                              !oboe::test::running(oboe::Direction::Output).empty(); }, 5s),
          "explicit opt-in starts direct loopback");
    {
        const auto cfg = readConfig();
        CHECK(cfg["selfHearing"] == true && std::abs(cfg["selfHearingVolume"].get<float>() - 0.05f) < 0.0001f &&
              cfg["micGainDb"] == -3.0, "v4 opt-in and chosen levels are preserved");
    }
    CHECK(mod.disable() && mod.unload(), "stop explicit opt-in");

    // Unknown future and broken configs fail closed, without losing user data.
    {
        std::ofstream future(configPath);
        future << R"({"version":5,"selfHearing":true,"secret":"keep-future-secret"})";
    }
    CHECK(!mod.load() && !mod.enable(), "newer config fails closed");
    CHECK(noRunningAudio() && readConfig()["version"] == 5 && readConfig()["secret"] == "keep-future-secret",
          "newer config remains untouched and silent");
    CHECK(mod.unload(), "unload after newer config");
    {
        std::ofstream broken(configPath);
        broken << "{";
    }
    CHECK(!mod.load() && !mod.enable(), "malformed config fails closed");
    {
        std::ifstream f(configPath);
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(noRunningAudio() && text == "{", "malformed config remains untouched and silent");
    }
    CHECK(mod.unload(), "unload after malformed config");

    // Internal audio defaults agree with the generated config, and invalid
    // playback values fail silent rather than becoming full-volume playback.
    {
        const VoiceSessionConfig defaults;
        const Loopback::Options options;
        CHECK(!defaults.selfHearing && std::abs(defaults.selfHearingVolume - 0.1f) < 0.0001f &&
              options.micGainDb == 0.0f && std::abs(options.playbackVolume - 0.1f) < 0.0001f,
              "internal monitoring defaults are conservative");

        VoiceSessionConfig invalid;
        invalid.micSource = "direct";
        invalid.selfHearing = true;
        invalid.selfHearingVolume = std::numeric_limits<float>::infinity();
        VoiceSession session(invalid, [] { return PlayerSnapshot{}; }, nullptr);
        CHECK(session.start() && session.startDirectMic(), "start session with invalid monitor volume");
        auto in = oboe::test::running(oboe::Direction::Input).back();
        auto out = oboe::test::running(oboe::Direction::Output).back();
        std::int16_t samples[320];
        for (auto &s : samples) s = 12000;
        in->testCallback(samples, 320);
        std::this_thread::sleep_for(100ms);
        std::vector<std::int16_t> played(320 * 2);
        out->testCallback(played.data(), 320);
        CHECK(energy(played) == 0, "invalid session monitor volume fails silent");
        session.stop();

        Loopback loop;
        Loopback::Options bad;
        bad.micGainDb = std::numeric_limits<float>::infinity();
        bad.playbackVolume = std::numeric_limits<float>::quiet_NaN();
        CHECK(loop.start(bad) && loop.volume() == 0.0f, "invalid loopback volume fails silent");
        loop.setVolume(std::numeric_limits<float>::infinity());
        CHECK(loop.volume() == 0.0f, "invalid volume change fails silent");
        loop.stop();
    }

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

#ifdef VOICECHAT_ENABLE_WEBRTC
    // The launcher's own Android AEC must not be doubled. WebRTC's optional
    // noise suppressor still receives its 20 ms microphone frames.
    {
        VoiceSessionConfig cfg;
        cfg.micSource = "launcher";
        cfg.webrtcVoiceAec = true;
        cfg.webrtcNoiseSuppression = true;
        cfg.sender.gateRms = 0;
        VoiceSession session(cfg, [] { return PlayerSnapshot{}; }, nullptr);
        CHECK(session.start(), "start launcher with WebRTC suppression");
        CHECK(session.status().find("voice-only AEC off") != std::string::npos &&
              session.status().find("WebRTC NS on") != std::string::npos,
              "launcher AEC is not doubled while NS is active");
        const int fd = connectLauncherMic();
        std::uint32_t rng = 0xa123b456u;
        for (int f = 0; f < 400; ++f) {
            std::int16_t samples[320];
            for (auto &sample : samples) {
                rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                sample = static_cast<std::int16_t>(static_cast<int>(rng % 5001) - 2500);
            }
            sendAll(fd, samples, sizeof samples);
        }
        std::this_thread::sleep_for(500ms);
        const auto status = session.status();
        const auto pos = status.find("mic level ");
        CHECK(pos != std::string::npos, "processed mic level is reported");
        const int rms = std::stoi(status.substr(pos + std::strlen("mic level ")));
        CHECK(rms > 20 && rms < 1100, "launcher synthetic stationary noise is processed rather than passed raw or muted");
        ::close(fd);
        session.stop();
    }

    // No partial software AEC alongside the Android communication preset or
    // self-monitoring. Neither choice implies that feedback is prevented.
    for (bool monitor : {false, true}) {
        VoiceSessionConfig cfg;
        cfg.micSource = "direct";
        cfg.webrtcVoiceAec = true;
        cfg.micInputPreset = monitor ? "voice_recognition" : "voice_communication";
        cfg.selfHearing = monitor;
        VoiceSession session(cfg, [] { return PlayerSnapshot{}; }, nullptr);
        CHECK(session.start(), "start safeguarded WebRTC configuration");
        CHECK(session.status().find("voice-only AEC off") != std::string::npos,
              "partial AEC is skipped for communication preset and self-monitoring");
        session.stop();
    }

    // Direct mode accepts irregular playback chunks as reference. Deliberately
    // overwhelm the bounded reference queue to ensure loss disables processing
    // and returns to raw mic instead of using a stale/misaligned reference.
    {
        VoiceSessionConfig cfg;
        cfg.micSource = "direct";
        cfg.webrtcVoiceAec = true;
        cfg.webrtcNoiseSuppression = true;
        cfg.sender.gateRms = 0;
        VoiceSession session(cfg, [] { return PlayerSnapshot{}; }, nullptr);
        CHECK(session.start() && session.startDirectMic(), "start experimental direct processing");
        CHECK(session.status().find("voice-only AEC on") != std::string::npos &&
              session.status().find("WebRTC NS on") != std::string::npos,
              "explicit direct processing is active");
        auto in = oboe::test::running(oboe::Direction::Input).back();
        auto out = oboe::test::running(oboe::Direction::Output).back();
        std::vector<std::int16_t> played(320 * 2);
        std::int16_t mic[320];
        for (auto &s : mic) s = 1000;
        for (int i = 0; i < 12; ++i) {
            out->testCallback(played.data(), 113);
            out->testCallback(played.data() + 226, 207);
            in->testCallback(mic, 320);
            std::this_thread::sleep_for(10ms);
        }
        CHECK(session.status().find("voice-only AEC on") != std::string::npos,
              "normal-sized reference blocks keep processing active");
        std::vector<std::int16_t> excessive(32000 * 2);
        out->testCallback(excessive.data(), 32000);
        CHECK(waitFor([&] { return session.status().find("processing bypassed after error") != std::string::npos; }, 2s),
              "reference overflow disables processing");
        for (auto &s : mic) s = 12000;
        in->testCallback(mic, 320);
        CHECK(waitFor([&] { return session.status().find("mic level 12000") != std::string::npos; }, 2s),
              "after overflow the original microphone signal is passed through");
        session.stop();
        CHECK(session.start() && session.startDirectMic(), "restart processor after reference overflow");
        CHECK(session.status().find("voice-only AEC on") != std::string::npos,
              "restart creates a fresh processor and reference queue");
        in = oboe::test::running(oboe::Direction::Input).back();
        std::vector<std::int16_t> excessiveMic(32000, 12000);
        in->testCallback(excessiveMic.data(), static_cast<int>(excessiveMic.size()));
        CHECK(waitFor([&] { return session.status().find("processing bypassed after error") != std::string::npos; }, 2s),
              "capture overflow also disables processing when timing is lost");
        CHECK(waitFor([&] { return session.status().find("mic level 12000") != std::string::npos; }, 2s),
              "after capture overflow microphone passes through unprocessed");
        session.stop();
        CHECK(session.start(), "restart processor after capture overflow");
        CHECK(session.status().find("voice-only AEC on") != std::string::npos,
              "restart clears capture overflow too");
        session.stop();
    }
    std::puts("WebRTC session wiring and safeguards OK (simulated callbacks)");
#else
    {
        VoiceSessionConfig cfg;
        cfg.micSource = "direct";
        cfg.webrtcVoiceAec = true;
        cfg.webrtcNoiseSuppression = true;
        VoiceSession session(cfg, [] { return PlayerSnapshot{}; }, nullptr);
        CHECK(session.start(), "start without compiled WebRTC");
        CHECK(session.status().find("voice-only AEC off") != std::string::npos &&
              session.status().find("WebRTC NS off") != std::string::npos,
              "uncompiled WebRTC is not reported as active");
        session.stop();
    }
#endif

    run = false;
    ticks.join(); peerPosition.join(); peer.stop();
    std::puts("VOICE MOD INTEGRATION OK");
}
