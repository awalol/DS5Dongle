//
// Elastic buffer between the controller's mic clock and the USB side.
//
// Decoded mic samples go in as they arrive, in Bluetooth bursts at the
// controller's clock. audio_loop() reads 48 samples per millisecond of the
// dongle clock, which the controller follows, and TinyUSB's IN flow control
// adjusts the packet sizes to the host.
//
// The ring is read with a fractional step. A PI loop on the ring level nudges
// the step so the reader consumes samples as fast as the writer adds them;
// linear interpolation is enough because the step stays within a few ppm of 1.
// After an underrun the reader waits until the ring is back at its target
// level, so one late burst costs one gap instead of a gap per USB frame.
//

#include "port/port.h"
#include "elastic.h"

#include <algorithm>
#include <cstring>

static constexpr uint64_t ONE_Q32 = 1ull << 32;
static constexpr uint32_t PULLS_PER_UPDATE = 10; // one control update per 10 ms of pulls

// Indices wrap at 2^32 frames. With a RING that is not a power of two the
// ring position jumps once at that wrap, after 24.8 hours of 48 kHz audio.
template<int CH, uint32_t RING>
struct Elastic {
    int16_t ring[RING * CH];
    uint32_t wr;            // frames written, wraps with the read index
    uint64_t rd;            // read position, Q32.32 frames
    uint64_t step = ONE_Q32;
    int64_t acc_q16;
    uint32_t acc_n;
    int64_t integ_q16;      // sum of level errors, one term per control update
    uint16_t target;        // frames to hold; the reader starts and restarts at this level
    uint32_t kp_ppb;        // proportional gain, ppb of rate per frame of level error
    uint16_t ti_s;          // integral time, seconds
    bool running;

    int64_t level_q32() const {
        return static_cast<int64_t>((static_cast<uint64_t>(wr) << 32) - rd);
    }

    void push(const int16_t *frames, int count) {
        for (int i = 0; i < count; i++) {
            memcpy(&ring[(wr % RING) * CH], frames + i * CH, CH * sizeof(int16_t));
            wr++;
        }
        if (level_q32() > static_cast<int64_t>(RING - 2) << 32) {
            // The reader stalled or the writer runs far ahead: back to the target level.
            rd = static_cast<uint64_t>(wr - target) << 32;
            running = false;
        }
    }

    bool try_start() {
        if (level_q32() < static_cast<int64_t>(target) << 32) return false;
        running = true;
        acc_q16 = 0;
        acc_n = 0;
        return true;
    }

    void control_update() {
        const int64_t err_q16 = acc_q16 / acc_n - (static_cast<int64_t>(target) << 16);
        acc_q16 = 0;
        acc_n = 0;
        // integral term: kp * sum(err) * update period / ti
        const int64_t updates_per_ti = static_cast<int64_t>(ti_s) * 100;
        const int64_t i_limit = (1000000LL << 16) * updates_per_ti / std::max<uint32_t>(kp_ppb, 1);
        integ_q16 = std::clamp(integ_q16 + err_q16, -i_limit, i_limit);
        const int64_t p = static_cast<int64_t>(kp_ppb) * err_q16 >> 16;
        const int64_t i = static_cast<int64_t>(kp_ppb) * integ_q16 / updates_per_ti >> 16;
        const int64_t ppb = std::clamp<int64_t>(p + i, -1000000, 1000000);
        step = ONE_Q32 + (ppb * static_cast<int64_t>(ONE_Q32)) / 1000000000;
    }

    // Reads count frames; false on underrun (out untouched, waits for the target again).
    bool pull(int16_t *out, int count) {
        if (level_q32() < static_cast<int64_t>(count + 2) << 32) {
            running = false;
            return false;
        }
        for (int n = 0; n < count; n++) {
            const auto idx = static_cast<uint32_t>(rd >> 32);
            const auto frac = static_cast<int32_t>((rd >> 16) & 0xFFFF);
            const int16_t *a = &ring[(idx % RING) * CH];
            const int16_t *b = &ring[((idx + 1) % RING) * CH];
            for (int c = 0; c < CH; c++) {
                out[n * CH + c] = static_cast<int16_t>(a[c] + (((b[c] - a[c]) * frac) >> 16));
            }
            rd += step;
        }
        acc_q16 += level_q32() >> 16;
        if (++acc_n == PULLS_PER_UPDATE) {
            control_update();
        }
        return true;
    }
};

// 6144 mono samples = 128 ms of static RAM. The bursts swing the level by about
// +-1200 samples (25 ms), so the target sits at 50 ms and the loop is slow: it
// only has to follow ppm-level drift.
static Elastic<1, 6144> mic{.target = 2400, .kp_ppb = 100, .ti_s = 60};

void PORT_FAST_FUNC(mic_elastic_write)(const int16_t *samples, int count) {
    mic.push(samples, count);
}

bool PORT_FAST_FUNC(mic_elastic_read)(int16_t *out, int count) {
    if (!mic.running && !mic.try_start()) return false;
    return mic.pull(out, count);
}
