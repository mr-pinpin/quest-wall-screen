// Streaming budget for headset assets (docs/3d-content.md): downloads take the BUDGET arbiter's asset share and
// nothing more, so models never steal from the game stream or the screens. A token bucket: `allow(now)` says how
// many bytes may be read now; unused allowance accumulates up to `burstMs` worth. Pure, tested in bench/model_test.cpp.
#pragma once
#include <algorithm>
#include <cstdint>

namespace assets {

struct Bucket {
    double tokens = 0, last = -1, burstMs = 250;
    int kbps = 20000;                       // rate (kilobits/s); 0 = paused (e.g. while a game streams)
    uint64_t taken = 0;
    // Bytes that may be read at time `now` (seconds); call take() with what was actually read.
    uint64_t allow(double now) {
        if (last < 0) last = now;
        double cap = kbps * 125.0 * burstMs / 1000.0;  // kbps * 1000 / 8 bytes per second
        tokens = std::min(cap, tokens + (now - last) * kbps * 125.0);
        last = now;
        return kbps > 0 ? (uint64_t)std::max(0.0, tokens) : 0;
    }
    void take(uint64_t n) { tokens -= (double)n; taken += n; }
};

}  // namespace assets
