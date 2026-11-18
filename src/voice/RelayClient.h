#pragma once
// UDP client for the internet voice relay (relay/server.py, protocol v2).
// Works across different networks: the client only ever sends first, so NAT needs no setup on
// the player's side; only the relay server needs a public address.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace voicechat {

struct RelayConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 24454;
    std::string room = "default";
    std::string secret;
    std::uint32_t playerId = 0;
};

struct ForwardedVoice {
    std::uint32_t senderId = 0;
    std::uint16_t seq = 0;
    std::int32_t dim = 0;
    float x = 0, y = 0, z = 0, yaw = 0;
    std::vector<std::uint8_t> opus;
};

class RelayClient {
  public:
    using LogFn = std::function<void(const std::string &)>;
    using VoiceFn = std::function<void(const ForwardedVoice &)>; // called on the client's worker thread

    RelayClient(RelayConfig cfg, LogFn log, VoiceFn onVoice);
    ~RelayClient();

    void start();
    void stop(); // idempotent; sends BYE if connected

    bool connected() const { return connected_.load(); }
    std::uint32_t connectCount() const { return connects_.load(); }
    std::uint64_t voiceReceived() const { return voiceIn_.load(); }

    // Both return false if not connected / send failed. Safe from any thread.
    bool sendPosition(std::int32_t dim, float x, float y, float z, float yaw);
    bool sendVoice(std::uint16_t seq, std::int32_t dim, float x, float y, float z, float yaw,
                   const std::uint8_t *opus, std::size_t n);

  private:
    void run();
    bool openSocket();
    void closeSocket();
    bool sendRaw(const std::vector<std::uint8_t> &pkt);
    void sendHello();
    void handle(const std::uint8_t *d, std::size_t n);
    void note(const std::string &key, const std::string &msg);

    RelayConfig cfg_;
    LogFn log_;
    VoiceFn onVoice_;

    std::mutex mu_; // guards fd_ and session_
    int fd_ = -1;
    std::uint64_t session_ = 0;

    std::atomic<bool> connected_{false};
    std::atomic<std::uint32_t> connects_{0};
    std::atomic<std::uint64_t> voiceIn_{0};

    std::mutex stopMu_;
    std::condition_variable stopCv_;
    bool stop_ = false;
    std::thread worker_;

    std::string lastNoteKey_; // worker thread only
    std::int64_t lastNoteAt_ = 0;
};

} // namespace voicechat
