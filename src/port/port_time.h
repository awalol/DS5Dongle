#pragma once

#include <cstdint>

// Monotonic time since boot. The firmware only ever uses timestamps as plain
// integers (deadline comparisons and elapsed-time maths), so the port API
// exposes integers rather than an opaque time type.

namespace port {

// Microseconds since boot, full width.
uint64_t now_us();

// Microseconds since boot, truncated. Callers use this only for short
// intervals where 32-bit wraparound (~71 minutes) is already handled.
uint32_t now_us32();

// Milliseconds since boot. Replaces to_ms_since_boot(get_absolute_time()).
uint32_t now_ms();

void delay_ms(uint32_t ms);
void delay_us(uint32_t us);

// Hint for a spin-wait body; may compile to nothing.
void spin_hint();

} // namespace port

#if defined(PORT_PLATFORM_PICO)
#include "pico/port_time_impl.h"
#elif defined(PORT_PLATFORM_ESP32S31)
#include "esp32s31/port_time_impl.h"
#endif
