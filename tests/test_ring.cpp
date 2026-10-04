// smallest check that fails if the SPSC ring handoff breaks.
// spa_ringbuffer is header-only; no PipeWire daemon needed.
// Build (no NDEBUG — asserts must live): g++ -I<spa include dir> tests/test_ring.cpp
#include <spa/utils/ringbuffer.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {
constexpr uint32_t kFrameBytes = 8; // stereo float
constexpr uint32_t kRingFrames = 4096;
constexpr uint32_t kRingBytes = kRingFrames * kFrameBytes;
constexpr uint32_t kReadFrames = 256;
} // namespace

int main() {
    struct spa_ringbuffer ring = {};
    std::vector<float> storage(kRingBytes / sizeof(float), 0.0f);
    std::atomic<bool> done = false;

    std::thread producer([&] {
        uint64_t frame = 0;
        for (int round = 0; round < 4000; ++round) {
            float buf[512 * 2];
            for (uint32_t f = 0; f < 512; ++f, ++frame) {
                buf[f * 2] = (float)frame;
                buf[f * 2 + 1] = (float)frame;
            }
            const uint32_t bytes = sizeof(buf);
            uint32_t w = 0;
            const int32_t used = spa_ringbuffer_get_write_index(&ring, &w);
            if (used >= 0 && used + bytes <= kRingBytes) {
                spa_ringbuffer_write_data(&ring, storage.data(), kRingBytes, w % kRingBytes, buf,
                                          bytes);
                spa_ringbuffer_write_update(&ring, w + bytes);
            }
            // ring full -> drop this quantum, same as the engine's RT path
        }
        done = true;
    });

    std::thread consumer([&] {
        uint64_t last = 0;
        bool first = true;
        uint64_t gaps = 0;
        float out[kReadFrames * 2];
        for (;;) {
            uint32_t r = 0;
            const int32_t avail = spa_ringbuffer_get_read_index(&ring, &r);
            if (avail <= 0) {
                if (done.load())
                    break;
                continue;
            }
            const uint32_t want = std::min<uint32_t>((uint32_t)avail, kReadFrames * kFrameBytes);
            spa_ringbuffer_read_data(&ring, storage.data(), kRingBytes, r % kRingBytes, out, want);
            spa_ringbuffer_read_update(&ring, r + want);

            for (uint32_t f = 0; f < want / kFrameBytes; ++f) {
                const auto v = (uint64_t)out[f * 2];
                assert(out[f * 2 + 1] == out[f * 2] && "channels stay in sync");
                if (!first && v != last + 1) {
                    gaps += v > last ? (v - last - 1) : 1;
                    assert(gaps < kRingFrames && "gap too large, ring corrupt");
                }
                first = false;
                last = v;
            }
        }
        printf("consumer done, frames seen=%llu gaps=%llu\n", (unsigned long long)(last + 1),
               (unsigned long long)gaps);
        assert(gaps == 0 && "no samples lost in flight");
    });

    producer.join();
    consumer.join();
    printf("ring test OK\n");
    return 0;
}