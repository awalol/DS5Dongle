#pragma once

// Force-included into libopus's CELT sources on ESP32-S31 only (see
// CMakeLists.txt beside this file).
//
// The float build of CELT spells several of its math macros with the double
// functions -- celt_sqrt(x) is (float)sqrt(x), celt_cos_norm(x) is
// (float)cos(...) -- and many constants are unsuffixed, so the expression
// around them is promoted to double. The S31's FPU is single precision only,
// so every such call and promotion runs in software (__muldf3, sqrt, cos ...).
// The arguments are floats and the results are cast back to float, so routing
// them to the single-precision functions keeps the arithmetic in hardware.
//
// SILK's float sources are deliberately not given this header: they do use
// double precision on purpose. Neither encoder mode used here runs SILK.

#include <math.h>

#define sqrt(x) sqrtf(x)
#define cos(x) cosf(x)
#define sin(x) sinf(x)
#define exp(x) expf(x)
#define log(x) logf(x)
#define floor(x) floorf(x)
#define fabs(x) fabsf(x)
#define pow(x, y) powf((x), (y))
#define acos(x) acosf(x)
#define atan(x) atanf(x)
#define atan2(y, x) atan2f((y), (x))
#define tanh(x) tanhf(x)
