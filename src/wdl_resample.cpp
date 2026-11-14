// Project-local specialization; keep the upstream WDL submodule unchanged.
// The speaker's 16/15 ratio selects WDL's ideal-phase stereo sinc path.
// Float accumulators avoid a software float-to-double conversion and a DCP
// wrapper call for every tap. WDL still calculates the source phase in double.
#include "resample.h"

template <class T1, class T2>
static inline void SincSample2N(T1 *outptr, const T1 *inptr, double fracpos,
                               const T2 *filter, int filtsz, int oversize);

template <>
inline void SincSample2N<float, float>(float *outptr, const float *inptr,
                                      double fracpos, const float *filter,
                                      int filtsz, int oversize)
{
    const int ifpos = static_cast<int>(fracpos * oversize + 0.5);
    const float *coeff = filter + (oversize - ifpos) * filtsz;
    float left = 0.0f;
    float right = 0.0f;
    for (int i = 0; i < filtsz / 2; ++i) {
        left += coeff[0] * inptr[0];
        right += coeff[0] * inptr[1];
        left += coeff[1] * inptr[2];
        right += coeff[1] * inptr[3];
        inptr += 4;
        coeff += 2;
    }
    outptr[0] = left;
    outptr[1] = right;
}

// Include WDL after the specialization so its inlined ResampleOut calls use
// the float implementation. Other sample types, channels and modes use WDL.
#include "../lib/WDL/WDL/resample.cpp"
