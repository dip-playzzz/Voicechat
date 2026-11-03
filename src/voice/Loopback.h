#pragma once
#include <memory>

namespace voicechat {

// Mic -> Opus encode -> Opus decode -> speaker. Proves audio + codec work
// before any networking or game hooks. Use headphones or you'll get feedback.
class Loopback {
  public:
    Loopback();
    ~Loopback();
    bool start();
    void stop();

  private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace voicechat
