//
// See speaker_resample.h.
//

#include "speaker_resample.h"

#include <cmath>
#include <cstring>

// A host test builds this file without the platform layer and defines the
// placement macro itself.
#ifndef PORT_FAST_FUNC
#include "port/port_mem.h"
#endif

namespace {

constexpr int kTaps = SpeakerResampler::kTaps;
constexpr int kPhases = SpeakerResampler::kPhases;
constexpr int kInFrames = SpeakerResampler::kInFrames;
constexpr int kOutFrames = SpeakerResampler::kOutFrames;
constexpr int kCarryMax = SpeakerResampler::kCarryMax;

// Frames of silence in front of the first block: half the filter, less one.
// It is what WDL_Resampler puts there, so the first output lines up with it.
constexpr int kPrime = kTaps / 2 - 1;

// The filter WDL_Resampler::BuildLowPass makes for 51200 -> 48000, 12 taps and
// 15 phases, value for value: the same expressions in the same order, in
// double, then stored as float. Runs once, at start.
void build_filter(float *cfout) {
    const double pi = 3.1415926535897932384626433832795;
    const double ratio = 51200.0 / 48000.0;
    // Going down in rate: the cutoff sits just under the output's Nyquist.
    const double filtpos = 1.0 / (ratio * 1.03);

    const double dwindowpos = 2.0 * pi / static_cast<double>(kTaps);
    const double dsincpos = pi * filtpos;
    const int half_taps = kTaps / 2;
    const int half_phases = kPhases / 2;

    // The first half of the table is computed; the second is its mirror.
    double filtpower = 0.0;
    float *ptrout = cfout;
    for (int slice = 0; slice <= half_phases; slice++) {
        const double frac = slice / static_cast<double>(kPhases);
        const int center_x = slice == 0 ? half_taps : -1;
        // kPhases is odd, so every slice here is a whole run of taps.
        for (int x = 0; x < kTaps; x++) {
            if (x == center_x) {
                *ptrout++ = 1.0f;
                continue;
            }
            const double xfrac = frac + x;
            const double windowpos = dwindowpos * xfrac;
            const double sincpos = dsincpos * (xfrac - half_taps);
            // Blackman-Harris times sinc.
            const double val = (0.35875 - 0.48829 * cos(windowpos) + 0.14128 * cos(2 * windowpos) -
                                0.01168 * cos(3 * windowpos)) *
                               sin(sincpos) / sincpos;
            filtpower += slice ? val * 2 : val;
            *ptrout++ = static_cast<float>(val);
        }
    }

    filtpower = kPhases / (filtpower + 1.0);
    const int n = kTaps * (kPhases + 1) / 2;
    for (int x = 0; x < n; x++) {
        cfout[x] = static_cast<float>(cfout[x] * filtpower);
    }
    for (int x = n, y = n - 1; y >= 0; ++x, --y) {
        cfout[x] = cfout[y];
    }
}

} // namespace

void speaker_resample_init(SpeakerResampler &r) {
    build_filter(r.coeffs);
    memset(r.buf, 0, sizeof(r.buf));
    r.carry = kPrime;
    r.phase = 0;
}

float *speaker_resample_input(SpeakerResampler &r) {
    return r.buf + r.carry * 2;
}

int PORT_FAST_FUNC(speaker_resample_process)(SpeakerResampler &r, float *out) {
    const int avail = r.carry + kInFrames;
    // An output sample reads kTaps input frames from its position on.
    const int last = avail - kTaps - 1;

    int pos = 0;
    int phase = r.phase;
    int produced = 0;

    while (produced < kOutFrames && pos < last) {
        const float *in = r.buf + pos * 2;
        const float *coeff = r.coeffs + (kPhases - phase) * kTaps;
        // The tap loop of src/wdl_resample.cpp, in the same order.
        float left = 0.0f;
        float right = 0.0f;
        for (int i = 0; i < kTaps / 2; ++i) {
            left += coeff[0] * in[0];
            right += coeff[0] * in[1];
            left += coeff[1] * in[2];
            right += coeff[1] * in[3];
            in += 4;
            coeff += 2;
        }
        out[0] = left;
        out[1] = right;
        out += 2;
        ++produced;

        // Sixteen fifteenths of an input frame: one frame, and one phase.
        ++pos;
        if (++phase == kPhases) {
            phase = 0;
            ++pos;
        }
    }

    // Keep what the next block's first samples still read.
    if (pos > avail) pos = avail;
    int carry = avail - pos;
    if (carry > kCarryMax) {
        // Cannot happen at this ratio. Drop the oldest so nothing is overrun.
        pos += carry - kCarryMax;
        carry = kCarryMax;
    }
    memmove(r.buf, r.buf + pos * 2, static_cast<size_t>(carry) * 2 * sizeof(float));
    r.carry = carry;
    r.phase = phase;
    return produced;
}
