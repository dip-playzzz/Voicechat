#include "voice/RelayClient.h"

#include "voice/Sha256.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <random>

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "wire format is little-endian");

namespace voicechat {

namespace {
enum : std::uint8_t { HELLO = 1, VOICE = 2, POS = 3, BYE = 4, ACCEPT = 5, REJECT = 6, VOICE_FWD = 7, PING = 8, PONG = 9 };
constexpr std::uint8_t kVersion = 2;
constexpr std::size_t kMaxPacket = 1200;

template <class T> void put(std::vector<std::uint8_t> &b, T v) {
    const auto *p = reinterpret_cast<const std::uint8_t *>(&v);
    b.insert(b.end(), p, p + sizeof(T));
}
template <class T> bool get(const std::uint8_t *d, std::size_t n, std::size_t &o, T &v) {
    if (o + sizeof(T) > n) return false;
    std::memcpy(&v, d + o, sizeof(T));
    o += sizeof(T);
    return true;
}
std::vector<std::uint8_t> header(std::uint8_t type) { return {'V', 'C', kVersion, type}; }

std::int64_t nowSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

const char *reasonText(int r) {
    switch (r) {
    case 1: return "wrong room secret";
    case 2: return "clock differs too much from the server (or replayed hello)";
    case 3: return "room is full";
    case 4: return "rate limited";
    case 5: return "server is full";
    case 6: return "malformed hello";
    case 7: return "server restarted (reconnecting)";
    default: return "unknown reason";
    }
}
} // namespace

RelayClient::RelayClient(RelayConfig cfg, LogFn log, VoiceFn onVoice)
    : cfg_(std::move(cfg)), log_(std::move(log)), onVoice_(std::move(onVoice)) {}

RelayClient::~RelayClient() { stop(); }

void RelayClient::start() {
    if (worker_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(stopMu_);
        stop_ = false;
    }
    worker_ = std::thread([this] { run(); });
}

void RelayClient::stop() {
    {
        std::lock_guard<std::mutex> lock(stopMu_);
        stop_ = true;
    }
    stopCv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (connected_.exchange(false)) {
        std::uint64_t s;
        { std::lock_guard<std::mutex> lock(mu_); s = session_; }
        auto pkt = header(BYE);
        put(pkt, s);
        sendRaw(pkt);
    }
    closeSocket();
}

void RelayClient::note(const std::string &key, const std::string &msg) {
    // Same message at most every 30 s so a bad config doesn't flood the log.
    const auto t = nowSec();
    if (key == lastNoteKey_ && t - lastNoteAt_ < 30) return;
    lastNoteKey_ = key;
    lastNoteAt_ = t;
    if (log_) log_(msg);
}

bool RelayClient::openSocket() {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *res = nullptr;
    const int rc = getaddrinfo(cfg_.host.c_str(), std::to_string(cfg_.port).c_str(), &hints, &res);
    if (rc != 0 || !res) {
        note("dns", "Relay: cannot resolve '" + cfg_.host + "': " + gai_strerror(rc));
        return false;
    }
    int fd = -1;
    for (addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        // connect() on UDP just fixes the peer: the kernel then drops datagrams from anyone else.
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        note("sock", "Relay: cannot open a UDP socket to " + cfg_.host);
        return false;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    std::lock_guard<std::mutex> lock(mu_);
    fd_ = fd;
    session_ = 0;
    return true;
}

void RelayClient::closeSocket() {
    std::lock_guard<std::mutex> lock(mu_);
    if (fd_ >= 0) { close(fd_); fd_ = -1; }
    session_ = 0;
}

bool RelayClient::sendRaw(const std::vector<std::uint8_t> &pkt) {
    std::lock_guard<std::mutex> lock(mu_);
    if (fd_ < 0) return false;
    return send(fd_, pkt.data(), pkt.size(), MSG_NOSIGNAL | MSG_DONTWAIT) == static_cast<ssize_t>(pkt.size());
}

void RelayClient::sendHello() {
    static thread_local std::mt19937_64 rng{std::random_device{}() ^ (std::uint64_t(std::random_device{}()) << 32)};
    const std::uint32_t ts = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    const std::uint64_t nonce = rng();

    std::string msg = "VC2HELLO";
    msg.append(reinterpret_cast<const char *>(&cfg_.playerId), 4);
    msg += cfg_.room;
    msg.append(reinterpret_cast<const char *>(&ts), 4);
    msg.append(reinterpret_cast<const char *>(&nonce), 8);
    const auto mac = hmacSha256(cfg_.secret, msg);

    auto pkt = header(HELLO);
    put(pkt, cfg_.playerId);
    put(pkt, static_cast<std::uint8_t>(cfg_.room.size()));
    pkt.insert(pkt.end(), cfg_.room.begin(), cfg_.room.end());
    put(pkt, ts);
    put(pkt, nonce);
    pkt.insert(pkt.end(), mac.begin(), mac.begin() + 16);
    sendRaw(pkt);
}

bool RelayClient::sendPosition(std::int32_t dim, float x, float y, float z, float yaw) {
    if (!connected_) return false;
    std::uint64_t s;
    { std::lock_guard<std::mutex> lock(mu_); s = session_; }
    auto pkt = header(POS);
    put(pkt, s); put(pkt, dim); put(pkt, x); put(pkt, y); put(pkt, z); put(pkt, yaw);
    return sendRaw(pkt);
}

bool RelayClient::sendVoice(std::uint16_t seq, std::int32_t dim, float x, float y, float z, float yaw,
                            const std::uint8_t *opus, std::size_t n) {
    if (!connected_ || !opus || n == 0 || n > 400) return false;
    std::uint64_t s;
    { std::lock_guard<std::mutex> lock(mu_); s = session_; }
    auto pkt = header(VOICE);
    put(pkt, s); put(pkt, seq); put(pkt, dim); put(pkt, x); put(pkt, y); put(pkt, z); put(pkt, yaw);
    put(pkt, static_cast<std::uint16_t>(n));
    pkt.insert(pkt.end(), opus, opus + n);
    return sendRaw(pkt);
}

void RelayClient::handle(const std::uint8_t *d, std::size_t n) {
    if (n < 4 || d[0] != 'V' || d[1] != 'C' || d[2] != kVersion) return;
    const std::uint8_t type = d[3];
    std::size_t o = 4;
    switch (type) {
    case ACCEPT: {
        std::uint64_t s;
        if (!get(d, n, o, s)) return;
        { std::lock_guard<std::mutex> lock(mu_); session_ = s; }
        const bool wasConnected = connected_.exchange(true);
        if (!wasConnected) {
            connects_++;
            if (log_) log_("Relay: connected to " + cfg_.host + ":" + std::to_string(cfg_.port) + " room '" + cfg_.room + "'");
        }
        break;
    }
    case REJECT: {
        std::uint8_t reason = 0; std::uint32_t serverTs = 0;
        if (!get(d, n, o, reason) || !get(d, n, o, serverTs)) return;
        if (reason == 7 && connected_) { // server forgot us (restart): say HELLO again
            connected_ = false;
            if (log_) log_(std::string("Relay: ") + reasonText(reason));
        } else if (reason != 7) {
            std::string extra;
            if (reason == 2) {
                const auto local = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                extra = " (your clock is off by " + std::to_string(static_cast<long long>(local) - serverTs) + " s)";
            }
            note("rej" + std::to_string(reason), std::string("Relay rejected us: ") + reasonText(reason) + extra);
        }
        break;
    }
    case VOICE_FWD: {
        ForwardedVoice v;
        std::uint16_t len = 0;
        if (!get(d, n, o, v.senderId) || !get(d, n, o, v.seq) || !get(d, n, o, v.dim) || !get(d, n, o, v.x) ||
            !get(d, n, o, v.y) || !get(d, n, o, v.z) || !get(d, n, o, v.yaw) || !get(d, n, o, len))
            return;
        if (o + len != n || len == 0 || len > 400) return;
        v.opus.assign(d + o, d + o + len);
        voiceIn_++;
        if (onVoice_) onVoice_(v);
        break;
    }
    default: break; // PONG and unknown: only counts as "server is alive"
    }
}

void RelayClient::run() {
    using clock = std::chrono::steady_clock;
    auto lastHello = clock::time_point{}, lastPing = clock::now(), lastRecv = clock::now();
    auto helloGap = std::chrono::milliseconds(1000);
    std::uint8_t buf[2048];

    std::unique_lock<std::mutex> lk(stopMu_);
    while (!stop_) {
        lk.unlock();

        int fd;
        { std::lock_guard<std::mutex> lock(mu_); fd = fd_; }
        if (fd < 0) {
            if (!openSocket()) {
                lk.lock();
                stopCv_.wait_for(lk, std::chrono::seconds(3), [this] { return stop_; });
                continue;
            }
            { std::lock_guard<std::mutex> lock(mu_); fd = fd_; }
            lastHello = clock::time_point{};
            helloGap = std::chrono::milliseconds(1000);
            lastRecv = clock::now();
        }

        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 100) > 0 && (p.revents & POLLIN)) {
            for (;;) {
                const ssize_t r = recv(fd, buf, sizeof buf, 0);
                if (r <= 0) break;
                lastRecv = clock::now();
                handle(buf, static_cast<std::size_t>(r));
            }
        }

        const auto now = clock::now();
        if (!connected_) {
            if (now - lastHello >= helloGap) {
                sendHello();
                lastHello = now;
                helloGap = std::min(helloGap * 2, std::chrono::milliseconds(5000)); // back off while unreachable
            }
            // no answer for a long time: re-resolve DNS / reopen the socket
            if (now - lastRecv > std::chrono::seconds(20)) closeSocket();
        } else {
            helloGap = std::chrono::milliseconds(1000);
            if (now - lastPing >= std::chrono::seconds(2)) {
                std::uint64_t s;
                { std::lock_guard<std::mutex> lock(mu_); s = session_; }
                auto pkt = header(PING);
                put(pkt, s);
                sendRaw(pkt);
                lastPing = now;
            }
            if (now - lastRecv > std::chrono::seconds(7)) { // server went silent
                connected_ = false;
                if (log_) log_("Relay: lost connection, reconnecting");
                closeSocket();
            }
        }
        lk.lock();
    }
}

} // namespace voicechat
