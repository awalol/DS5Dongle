// Host check of src/speaker_resample.cpp against WDL_Resampler, which it
// replaces on the speaker path. Built and run by tests/test_speaker_resample.py.
//
// Both are fed the same blocks. They must give the same number of frames for
// every block and the same samples, bit for bit, wherever the two agree on
// which input frame an output sample starts from.
//
// They can disagree on that only for the one output sample in fifteen that
// falls exactly on an input sample. WDL reaches that position by adding 16/15
// to a double over and over, so it can arrive a hair under the whole number
// and start one frame earlier with the last phase, where the whole-number
// position here starts on the frame with the first phase. Those are the same
// filter shifted by one tap: twelve taps over a symmetric kernel thirteen wide,
// and the tap at either end is the window's edge, six parts in a hundred
// thousand. So such a sample may differ, by a few millionths of full scale for
// full-scale input (-100 dB), and no other sample may differ at all.

#include "resample.h"
#include "speaker_resample.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

uint32_t lcg_state = 0x1234567u;

float noise() {
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return static_cast<float>(static_cast<int32_t>(lcg_state)) / 2147483648.0f;
}

} // namespace

int main() {
    WDL_Resampler wdl;
    wdl.SetMode(false, 0, true, 12, 15);
    wdl.SetRates(51200, 48000);
    wdl.SetFeedMode(true);
    wdl.Prealloc(2, 512, 480);

    static SpeakerResampler ours;
    speaker_resample_init(ours);

    const int blocks = 2000; // 20 seconds of speaker audio
    static float in[512 * 2];
    static float out_wdl[480 * 2];
    static float out_ours[480 * 2];

    long long frames = 0;
    long long differing = 0;
    long long differing_off_grid = 0;
    double worst = 0.0;
    int failures = 0;

    for (int block = 0; block < blocks; block++) {
        // Full-scale noise on the left, a 1 kHz tone with noise under it on
        // the right, and now and then a block of silence or of steps.
        for (int i = 0; i < 512; i++) {
            const double t = (block * 512.0 + i) / 51200.0;
            float left = noise();
            float right = static_cast<float>(0.8 * sin(2.0 * 3.14159265358979323846 * 1000.0 * t)) + 0.1f * noise();
            if (block % 97 == 13) left = right = 0.0f;
            if (block % 101 == 7) left = right = (i & 64) ? 1.0f : -1.0f;
            in[i * 2] = left;
            in[i * 2 + 1] = right;
        }

        WDL_ResampleSample *wdl_in;
        const int want = wdl.ResamplePrepare(512, 2, &wdl_in);
        if (want != 512) {
            printf("block %d: WDL asked for %d frames, not 512\n", block, want);
            return 1;
        }
        memcpy(wdl_in, in, sizeof(in));
        const int n_wdl = wdl.ResampleOut(out_wdl, 512, 480, 2);

        memcpy(speaker_resample_input(ours), in, sizeof(in));
        const int n_ours = speaker_resample_process(ours, out_ours);

        if (n_wdl != n_ours) {
            printf("block %d: WDL gave %d frames, ours %d\n", block, n_wdl, n_ours);
            failures++;
            continue;
        }
        const int expected = block == 0 ? 473 : 480;
        if (n_ours != expected) {
            printf("block %d: %d frames, expected %d\n", block, n_ours, expected);
            failures++;
        }

        for (int i = 0; i < n_ours; i++, frames++) {
            // The phase starts at 0 and advances one step each output frame.
            const bool on_grid = frames % 15 == 0;
            for (int ch = 0; ch < 2; ch++) {
                const float a = out_wdl[i * 2 + ch];
                const float b = out_ours[i * 2 + ch];
                if (memcmp(&a, &b, sizeof(a)) == 0) continue;
                differing++;
                if (!on_grid) differing_off_grid++;
                const double d = fabs(static_cast<double>(a) - static_cast<double>(b));
                if (d > worst) worst = d;
            }
        }
    }

    printf("frames %lld, samples %lld\n", frames, frames * 2);
    printf("samples that differ from WDL: %lld, all on an input sample: %s\n", differing,
           differing_off_grid == 0 ? "yes" : "NO");
    printf("largest difference: %.3g of full scale\n", worst);

    if (differing_off_grid != 0) {
        printf("FAIL: %lld samples differ where the two start from the same input frame\n", differing_off_grid);
        failures++;
    }
    if (worst > 1e-5) {
        printf("FAIL: a difference of %.3g is more than the filter's edge taps can make\n", worst);
        failures++;
    }
    if (failures) return 1;
    printf("OK\n");
    return 0;
}
