#include "voice/MicSocketSource.h"

#include <cerrno>
#include <cstddef>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "launcher PCM is little-endian");

namespace voicechat {

namespace {
constexpr std::size_t kFrameBytes = MicSocketSource::kFrameSamples * sizeof(int16_t);
}

MicSocketSource::MicSocketSource(std::string abstractName, FrameFn onFrame, LogFn log)
    : name_(std::move(abstractName)), onFrame_(std::move(onFrame)), log_(std::move(log)) {}

MicSocketSource::~MicSocketSource() { stop(); }

bool MicSocketSource::start() {
    if (thread_.joinable()) return true;
    auto fail = [this](const std::string &why) {
        if (log_) log_(why);
        for (int *fd : {&listenFd_, &wake_[0], &wake_[1]}) {
            if (*fd >= 0) { ::close(*fd); *fd = -1; }
        }
        return false;
    };

    if (::pipe2(wake_, O_CLOEXEC) != 0) return fail(std::string("pipe2 failed: ") + std::strerror(errno));
    listenFd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listenFd_ < 0) return fail(std::string("socket() failed: ") + std::strerror(errno));

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (name_.empty() || name_.size() + 1 > sizeof(addr.sun_path)) return fail("bad socket name");
    std::memcpy(addr.sun_path + 1, name_.data(), name_.size()); // sun_path[0] == '\0' -> abstract namespace
    const socklen_t len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name_.size());

    if (::bind(listenFd_, reinterpret_cast<sockaddr *>(&addr), len) != 0) {
        const int e = errno;
        return fail("cannot listen on @" + name_ + ": " + std::strerror(e) +
                    (e == EADDRINUSE ? " (name already in use - another instance or app?)" : ""));
    }
    if (::listen(listenFd_, 1) != 0) return fail(std::string("listen() failed: ") + std::strerror(errno));

    stop_ = false;
    frames_ = 0;
    thread_ = std::thread([this] { run(); });
    if (log_) log_("listening for the launcher microphone stream on @" + name_);
    return true;
}

void MicSocketSource::stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    const char b = 'x';
    (void)!::write(wake_[1], &b, 1); // wake poll() immediately
    thread_.join();
    for (int *fd : {&listenFd_, &wake_[0], &wake_[1]}) {
        if (*fd >= 0) { ::close(*fd); *fd = -1; }
    }
    connected_ = false;
}

void MicSocketSource::run() {
    while (!stop_) {
        pollfd fds[2] = {{listenFd_, POLLIN, 0}, {wake_[0], POLLIN, 0}};
        const int r = ::poll(fds, 2, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[1].revents) break;
        if (!(fds[0].revents & POLLIN)) continue;

        const int client = ::accept4(listenFd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) continue;

        // Only accept audio from our own app (same UID): the launcher service lives in the
        // same app as this mod, so any other UID is somebody else trying to feed us data.
        ucred cred{};
        socklen_t cl = sizeof cred;
        if (::getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cred, &cl) != 0 || cred.uid != ::getuid()) {
            if (log_) log_("rejected a connection from another app/user");
            ::close(client);
            continue;
        }

        if (log_) log_("launcher connected - receiving 16 kHz mono PCM");
        connected_ = true;
        serve(client);
        connected_ = false;
        ::close(client);
        if (log_ && !stop_) log_("launcher disconnected (it will reconnect)");
    }
}

void MicSocketSource::serve(int client) {
    unsigned char buf[4096];
    std::size_t fill = 0;
    while (!stop_) {
        pollfd fds[2] = {{client, POLLIN, 0}, {wake_[0], POLLIN, 0}};
        const int r = ::poll(fds, 2, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (fds[1].revents) return;
        if (!(fds[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;

        const ssize_t n = ::recv(client, buf + fill, sizeof buf - fill, 0);
        if (n <= 0) return; // closed or error
        fill += static_cast<std::size_t>(n);

        std::size_t off = 0;
        while (fill - off >= kFrameBytes) {
            int16_t samples[kFrameSamples];
            std::memcpy(samples, buf + off, kFrameBytes);
            off += kFrameBytes;
            frames_++;
            if (onFrame_) onFrame_(samples, kFrameSamples);
        }
        if (off) {
            std::memmove(buf, buf + off, fill - off);
            fill -= off;
        }
    }
}

} // namespace voicechat
