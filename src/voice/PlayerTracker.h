#pragma once
// Reads the local player's position / rotation / dimension by subscribing to
// BedrockTools' LocalPlayerTick event. Requires the BedrockTools mod to be loaded.
//
// Lifetime rules (see PlayerTracker.cpp):
//  * The callback registered with BedrockTools only touches a process-lifetime
//    "Shared" object, never `this`, so a late/in-flight callback can't hit freed memory.
//  * Only ONE PlayerTracker may be running at a time.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace bedrocktools::api { struct ApiV1; }

namespace voicechat {

struct PlayerSnapshot {
    bool valid = false; // false until the first tick has been seen
    float x = 0, y = 0, z = 0;
    float pitch = 0, yaw = 0; // degrees, Minecraft convention (yaw 0 = facing +Z)
    int dimension = 0;        // 0 overworld, 1 nether, 2 end
    std::uint64_t ticks = 0;
};

class PlayerTracker {
  public:
    using LogFn = std::function<void(const std::string &)>;

    // `runtime` is only for tests; normally null (found via libBedrockTools.so).
    PlayerTracker(LogFn log, bool periodicLog, const bedrocktools::api::ApiV1 *runtime = nullptr);
    ~PlayerTracker();

    // Starts a background thread that keeps trying to attach to BedrockTools
    // (it may load after us) and optionally logs the position every 5 s.
    void start();
    // Idempotent. Returns only after the worker has exited and the subscription is removed.
    void stop();

    // Safe from any thread EXCEPT the audio callback (takes a mutex).
    PlayerSnapshot snapshot() const;
    bool attached() const { return attached_.load(); }

  private:
    bool tryAttach(std::string &diag);
    void run();

    LogFn log_;
    bool periodicLog_;
    const bedrocktools::api::ApiV1 *testRuntime_;
    const bedrocktools::api::ApiV1 *rt_ = nullptr; // runtime we subscribed on (worker/stop only)
    std::uint64_t sub_ = 0;

    std::atomic<bool> attached_{false};
    std::mutex cvMu_;
    std::condition_variable cv_;
    bool stop_ = false; // guarded by cvMu_
    std::thread worker_;
};

} // namespace voicechat
