//
// RAM-resident memcpy / memset / memmove.
//
// On RP2350 the Pico SDK uses newlib's mem ops, which live in flash (the pico/
// bootrom mem-ops variant does not link -- RP2350's bootrom no longer exports
// ROM_FUNC_MEMCPY/MEMSET). core1's audio loop calls these every frame, directly
// and inside libopus, so without RAM copies core1 still executes flash code
// during steady-state audio. These overrides complete the move of core1's audio
// loop fully into RAM, removing the last per-frame XIP fetches on that core.
//
// These strong definitions override the newlib ones at link time and are placed
// in .time_critical (RAM) via __not_in_flash_func. The translation unit MUST be
// built with -fno-builtin and -fno-tree-loop-distribute-patterns (see
// CMakeLists.txt) so the word loops are not lowered back into memcpy/memset
// calls, which would self-recurse. Word-at-a-time on the common aligned path.
//

#include <stddef.h>
#include <stdint.h>
#include "port/port_mem.h"

void *PORT_FAST_FUNC(memcpy)(void *restrict dst, const void *restrict src, size_t n) {
    uint8_t *d = (uint8_t *) dst;
    const uint8_t *s = (const uint8_t *) src;
    if ((((uintptr_t) d | (uintptr_t) s) & 3u) == 0u) {
        uint32_t *dw = (uint32_t *) d;
        const uint32_t *sw = (const uint32_t *) s;
        for (size_t w = n >> 2; w; --w) *dw++ = *sw++;
        d = (uint8_t *) dw;
        s = (const uint8_t *) sw;
        n &= 3u;
    }
    while (n--) *d++ = *s++;
    return dst;
}

void *PORT_FAST_FUNC(memset)(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *) dst;
    const uint8_t b = (uint8_t) c;
    if (((uintptr_t) d & 3u) == 0u) {
        const uint32_t w = (uint32_t) b * 0x01010101u;
        uint32_t *dw = (uint32_t *) d;
        for (size_t i = n >> 2; i; --i) *dw++ = w;
        d = (uint8_t *) dw;
        n &= 3u;
    }
    while (n--) *d++ = b;
    return dst;
}

void *PORT_FAST_FUNC(memmove)(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *) dst;
    const uint8_t *s = (const uint8_t *) src;
    if (d == s || n == 0u) return dst;
    if (d < s) {
        if ((((uintptr_t) d | (uintptr_t) s) & 3u) == 0u) {
            uint32_t *dw = (uint32_t *) d;
            const uint32_t *sw = (const uint32_t *) s;
            for (size_t w = n >> 2; w; --w) *dw++ = *sw++;
            d = (uint8_t *) dw;
            s = (const uint8_t *) sw;
            n &= 3u;
        }
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}
