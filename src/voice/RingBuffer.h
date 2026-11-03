#pragma once
// Single-producer / single-consumer lock-free ring buffer (safe for audio threads).
#include <atomic>
#include <cstddef>
#include <vector>

template <typename T>
class SpscRing {
public:
    explicit SpscRing(size_t capacity) : buf_(capacity + 1) {}

    size_t write(const T* src, size_t n) {
        size_t w = w_.load(std::memory_order_relaxed), r = r_.load(std::memory_order_acquire);
        size_t free = (r + buf_.size() - w - 1) % buf_.size();
        if (n > free) n = free;
        for (size_t i = 0; i < n; i++) buf_[(w + i) % buf_.size()] = src[i];
        w_.store((w + n) % buf_.size(), std::memory_order_release);
        return n;
    }

    size_t read(T* dst, size_t n) {
        size_t r = r_.load(std::memory_order_relaxed), w = w_.load(std::memory_order_acquire);
        size_t avail = (w + buf_.size() - r) % buf_.size();
        if (n > avail) n = avail;
        for (size_t i = 0; i < n; i++) dst[i] = buf_[(r + i) % buf_.size()];
        r_.store((r + n) % buf_.size(), std::memory_order_release);
        return n;
    }

private:
    std::vector<T> buf_;
    std::atomic<size_t> w_{0}, r_{0};
};
