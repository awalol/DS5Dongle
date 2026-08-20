#pragma once

#include "pico.h"
#include "pico/time.h"

namespace port {

inline uint64_t now_us() { return time_us_64(); }
inline uint32_t now_us32() { return time_us_32(); }
inline uint32_t now_ms() { return to_ms_since_boot(get_absolute_time()); }

inline void delay_ms(uint32_t ms) { sleep_ms(ms); }
inline void delay_us(uint32_t us) { sleep_us(us); }

inline void spin_hint() { tight_loop_contents(); }

} // namespace port
