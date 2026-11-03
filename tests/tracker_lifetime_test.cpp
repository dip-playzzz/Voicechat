// Hammers the tracker's start/stop/destroy path while another thread keeps publishing
// LocalPlayerTick events through a real BedrockTools EventBus, wired up exactly like
// BedrockTools' own src/core/Api.cpp (snapshot-then-call-without-lock semantics).
// Build with -DVOICECHAT_TEST and run under ASan/UBSan and TSan.
#include "voice/PlayerTracker.h"

#include <bedrocktools/Api.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace bt = bedrocktools;

bt::events::EventBus &bt::events::bus() { static EventBus b; return b; }

static std::uint64_t t_subscribe(bt::events::EventType type, bt::events::EventPriority pr,
                                 bt::api::EventCallback cb, void *ud) {
    if (!cb) return 0;
    return bt::events::bus().subscribeRaw(type, [type, cb, ud](void *p) { cb(type, p, ud); }, pr);
}
static void t_unsubscribe(std::uint64_t id) { bt::events::bus().unsubscribe(id); }

static const bt::api::ApiV1 kRuntime{bt::api::AbiVersion, sizeof(bt::api::ApiV1), nullptr, nullptr,
                                     t_subscribe, t_unsubscribe};
#ifdef OLD_IMPL
namespace voicechat { extern const bt::api::ApiV1 *g_oldRuntime; }
#endif

int main(int argc, char **argv) {
    const int iterations = argc > 1 ? std::atoi(argv[1]) : 3000;

    voicechat::PlayerSnapshot fake;
    fake.x = 12.5f; fake.y = 64; fake.z = -3; fake.yaw = 90; fake.dimension = 1;
    bt::events::LocalPlayerTickEvent ev{reinterpret_cast<bt::sdk::Player *>(&fake)};

    std::atomic<bool> run{true};
    std::thread publisher([&] { while (run) bt::events::bus().publish(ev); });

    auto make = []() {
#ifdef OLD_IMPL
        voicechat::g_oldRuntime = &kRuntime;
        return new voicechat::PlayerTracker(nullptr, false);
#else
        return new voicechat::PlayerTracker(nullptr, false, &kRuntime);
#endif
    };

    // 1) functional check: snapshot reflects published events
    {
        auto *t = make();
        t->start();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!t->snapshot().valid && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        auto s = t->snapshot();
        if (!s.valid || s.x != 12.5f || s.dimension != 1) { puts("FAIL: snapshot not updated"); return 1; }
        delete t;
    }

    // 2) lifetime hammer: create/attach/destroy while events keep flowing
    for (int i = 0; i < iterations; i++) {
        auto *t = make();
        t->start();
        while (!t->attached()) std::this_thread::yield();
        if (i % 3 == 0) std::this_thread::yield();
        delete t; // callbacks may still be in flight on the publisher thread
    }

    run = false;
    publisher.join();
    printf("lifetime test ok (%d iterations)\n", iterations);
}
