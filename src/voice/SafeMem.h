#pragma once
// Crash-proof reads of game memory.
//
// Reading game structures at hard-coded offsets is only valid for the exact Minecraft build
// those offsets were made for. On any other build the "pointer" we load can be garbage, and
// dereferencing it kills the whole game with SIGSEGV (this is what crashed on 1.26.52).
// safeRead() asks the kernel to do the copy (write() into a pipe), which reports EFAULT
// for a bad address instead of delivering SIGSEGV to us.
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace voicechat {

inline bool safeRead(const void *addr, void *out, std::size_t n) {
    if (!addr || !out || n == 0 || n > 4096) return false;
    static std::mutex mu;
    static int fds[2] = {-1, -1};
    std::lock_guard<std::mutex> lock(mu);
    if (fds[0] < 0 && pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) {
        fds[0] = fds[1] = -1;
        return false;
    }
    const ssize_t w = write(fds[1], addr, n);
    if (w < 0) return false;
    if (static_cast<std::size_t>(w) != n) { // ran into unmapped memory part-way: drain and fail
        char scratch[4096];
        ssize_t left = w;
        while (left > 0) {
            const ssize_t r = read(fds[0], scratch, static_cast<std::size_t>(left));
            if (r <= 0) break;
            left -= r;
        }
        return false;
    }
    return read(fds[0], out, n) == static_cast<ssize_t>(n);
}

inline bool safeReadPtr(std::uintptr_t addr, std::uintptr_t &out) {
    return safeRead(reinterpret_cast<const void *>(addr), &out, sizeof(out));
}

// Rejects values that can't be a real player state (a wrong-offset read that happens to hit
// mapped memory returns noise, not a crash; this filters most of that noise).
inline bool plausiblePlayerSample(const float pos[3], const float rot[2]) {
    for (int i = 0; i < 3; i++) if (!std::isfinite(pos[i])) return false;
    for (int i = 0; i < 2; i++) if (!std::isfinite(rot[i])) return false;
    if (std::fabs(pos[0]) > 3.0e7f || std::fabs(pos[2]) > 3.0e7f) return false;
    if (pos[1] < -2000.f || pos[1] > 5000.f) return false;
    if (std::fabs(rot[0]) > 180.f) return false;   // pitch
    if (std::fabs(rot[1]) > 3600.f) return false;  // yaw
    return true;
}

} // namespace voicechat
