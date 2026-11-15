//
// The speaker path's resampler: 51.2 kHz stereo in, 48 kHz stereo out.
//
// 512 input frames become 480 output frames, a ratio of exactly 16 to 15. The
// filter is the one WDL_Resampler builds for that pair of rates in its sinc
// mode with 12 taps: a Blackman-Harris windowed sinc in 15 phases, one for
// each position an output sample can take between two input samples.
//
// What differs from WDL_Resampler is how the position of each output sample
// is kept. WDL keeps it as a double and works out the input index and the
// filter phase from it for every sample: a float-to-int, an int-to-double, a
// subtract, a multiply and two adds, all in double precision. On a processor
// without a double-precision FPU (the ESP32-S31's is single precision) each of
// those is a library call, 48,000 times a second.
//
// Because the ratio is 16 to 15, none of it is needed. The position advances
// by one input frame and one fifteenth each output frame, so it is kept as a
// whole input index and a phase from 0 to 14: an add, a compare and, once in
// fifteen, a second add. It is exact, where the running double sum is not.
// The taps are single precision, as in src/wdl_resample.cpp.
//

#pragma once

struct SpeakerResampler {
    static constexpr int kInFrames = 512;
    static constexpr int kOutFrames = 480;
    static constexpr int kTaps = 12;
    static constexpr int kPhases = 15;
    // Frames kept from one block to the next. In steady state it is 13; the
    // room is for the first block, and so an error cannot write past the end.
    static constexpr int kCarryMax = 32;

    // kPhases + 1 runs of kTaps: phase p uses the run at (kPhases - p).
    float coeffs[kTaps * (kPhases + 1)];
    // Interleaved stereo: the frames carried over, then the block being fed.
    float buf[(kCarryMax + kInFrames) * 2];
    int carry;
    int phase;
};

// Build the filter and start from silence. Call once before the first block,
// and again to start over.
void speaker_resample_init(SpeakerResampler &r);

// Where to write the next block: room for kInFrames interleaved stereo frames.
float *speaker_resample_input(SpeakerResampler &r);

// Resample the block just written. Writes up to kOutFrames interleaved stereo
// frames to out and returns how many. Every block gives kOutFrames except the
// first after speaker_resample_init, which gives 473: the filter's first six
// taps have no history to read.
int speaker_resample_process(SpeakerResampler &r, float *out);
