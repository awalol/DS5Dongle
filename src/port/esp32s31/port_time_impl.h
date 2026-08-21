#pragma once

#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace port {

inline uint64_t now_us() { return static_cast<uint64_t>(esp_timer_get_time()); }
inline uint32_t now_us32() { return static_cast<uint32_t>(esp_timer_get_time()); }
inline uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

inline void delay_ms(uint32_t ms) {
    // Sub-tick sleeps would round down to zero and spin; busy-wait those so the
    // caller still gets the delay it asked for.
    const uint32_t tick_ms = portTICK_PERIOD_MS;
    if (ms >= tick_ms) {
        vTaskDelay(ms / tick_ms);
        ms %= tick_ms;
    }
    if (ms) esp_rom_delay_us(ms * 1000);
}

inline void delay_us(uint32_t us) { esp_rom_delay_us(us); }

inline void spin_hint() { __asm__ __volatile__("nop"); }

} // namespace port
