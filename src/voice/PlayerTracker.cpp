#include "voice/PlayerTracker.h"

#include <bedrocktools/Api.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <bedrocktools/sdk/world/BlockSource.hpp>

#include <chrono>

namespace voicechat {

namespace bt = bedrocktools;

namespace {

// BedrockTools' event bus copies its listener list and then calls the copies
// WITHOUT holding its lock, so a tick that started before unsubscribe() can still
// invoke our callback afterwards. Therefore everything the callback touches lives
// here, in an object that is intentionally never destroyed, and `active` turns
// late calls into no-ops. The callback never touches PlayerTracker itself.
struct Shared {
    std::mutex mu;
    PlayerSnapshot snap;
    std::atomic<bool> active{false};
};

Shared &shared() {
    static Shared *s = new Shared; // leaked on purpose
    return *s;
}

#ifdef VOICECHAT_TEST
// Test seam: `p` points at a PlayerSnapshot instead of a real game Player.
bool readPlayer(bt::sdk::Player *p, PlayerSnapshot &out) {
    if (!p) return false;
    out = *reinterpret_cast<const PlayerSnapshot *>(p);
    out.valid = true;
    return true;
}
#else
bool readPlayer(bt::sdk::Player *p, PlayerSnapshot &out) {
    if (!p) return false;
    const auto pos = p->position();
    const auto rot = p->rotation(); // x = pitch, y = yaw (same use as BedrockTools' Compass)
    int dim = 0;
    if (auto *dimension = p->dimension()) {
        if (auto *region = dimension->blockSource()) dim = region->dimensionId();
    }
    out.valid = true;
    out.x = pos.x; out.y = pos.y; out.z = pos.z;
    out.pitch = rot.x; out.yaw = rot.y;
    out.dimension = dim;
    return true;
}
#endif

// Plain function (no captures): `user` is the immortal Shared.
void onTick(bt::events::EventType type, void *payload, void *user) {
    auto *s = static_cast<Shared *>(user);
    if (!s->active.load(std::memory_order_acquire)) return;
    if (type != bt::events::EventType::LocalPlayerTick || !payload) return;

    PlayerSnapshot next;
    if (!readPlayer(static_cast<bt::events::LocalPlayerTickEvent *>(payload)->player, next)) return;

    std::lock_guard<std::mutex> lock(s->mu);
    if (!s->active.load(std::memory_order_relaxed)) return; // stop() ran meanwhile
    next.ticks = s->snap.ticks + 1;
    s->snap = next;
}

} // namespace

PlayerTracker::PlayerTracker(LogFn log, bool periodicLog, const bt::api::ApiV1 *runtime)
    : log_(std::move(log)), periodicLog_(periodicLog), testRuntime_(runtime) {}

PlayerTracker::~PlayerTracker() { stop(); }

bool PlayerTracker::tryAttach() {
    // find() looks up libBedrockTools.so with RTLD_NOLOAD, so it returns null
    // until BedrockTools has been loaded into the process.
    const bt::api::ApiV1 *rt = testRuntime_ ? testRuntime_ : bt::api::find();
    if (!bt::api::compatible(rt) || !rt->subscribe || !rt->unsubscribe) return false;

    {
        std::lock_guard<std::mutex> lock(shared().mu);
        shared().snap = PlayerSnapshot{};
    }
    shared().active.store(true, std::memory_order_release);
    const std::uint64_t id = rt->subscribe(bt::events::EventType::LocalPlayerTick,
                                           bt::events::EventPriority::Normal, onTick, &shared());
    if (id == 0) {
        shared().active.store(false, std::memory_order_release);
        return false;
    }
    rt_ = rt;
    sub_ = id;
    return true;
}

void PlayerTracker::start() {
    if (worker_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(cvMu_);
        stop_ = false;
    }
    worker_ = std::thread([this] { run(); });
}

void PlayerTracker::stop() {
    {
        std::lock_guard<std::mutex> lock(cvMu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join(); // after this, only this thread touches rt_/sub_

    // 1) make any in-flight or late callback a no-op, 2) then unsubscribe.
    shared().active.store(false, std::memory_order_release);
    if (sub_ != 0) {
        // Only call into BedrockTools if it's still the same loaded instance
        // (it may already have been unloaded during shutdown).
        const bt::api::ApiV1 *live = testRuntime_ ? testRuntime_ : bt::api::find();
        if (live == rt_ && live->unsubscribe) live->unsubscribe(sub_);
        sub_ = 0;
        rt_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(shared().mu);
        shared().snap = PlayerSnapshot{};
    }
    attached_ = false;
}

void PlayerTracker::run() {
    using namespace std::chrono;
    bool warned = false;
    auto lastLog = steady_clock::now();
    std::unique_lock<std::mutex> lk(cvMu_);
    while (!stop_) {
        lk.unlock();
        if (!attached_) {
            if (tryAttach()) {
                attached_ = true;
                if (log_) log_("Attached to BedrockTools player events");
            } else if (!warned && log_) {
                warned = true;
                log_("BedrockTools not loaded yet - make sure the BedrockTools mod is imported and enabled; retrying");
            }
        } else if (periodicLog_ && steady_clock::now() - lastLog >= seconds(5)) {
            lastLog = steady_clock::now();
            const auto s = snapshot();
            if (log_) {
                log_(s.valid ? "player pos=(" + std::to_string(s.x) + ", " + std::to_string(s.y) + ", " +
                                   std::to_string(s.z) + ") yaw=" + std::to_string(s.yaw) +
                                   " dim=" + std::to_string(s.dimension)
                             : std::string("attached, waiting for first player tick (not in a world yet?)"));
            }
        }
        lk.lock();
        cv_.wait_for(lk, milliseconds(500), [this] { return stop_; }); // wakes immediately on stop()
    }
}

PlayerSnapshot PlayerTracker::snapshot() const {
    std::lock_guard<std::mutex> lock(shared().mu);
    return shared().snap;
}

} // namespace voicechat
