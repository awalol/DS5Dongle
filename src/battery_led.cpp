//
// Low-battery LED indicator. See battery_led.h.
//

#include "battery_led.h"

#include <cstdint>

#include "config.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"

extern uint8_t interrupt_in_data[63];

namespace {

constexpr uint64_t REPORT_STALE_US = 2'000'000;  // assume disconnected if no report for 2 s
constexpr uint64_t BLINK_PERIOD_US =   500'000;  // 1 Hz, 50% duty
constexpr uint8_t  THRESHOLD_LEVEL = 1;          // PowerPercent <= 1 (i.e. <= 10%)
constexpr uint8_t  POWER_STATE_DISCHARGING = 0x0;

uint64_t last_report_us = 0;
uint64_t last_toggle_us = 0;
bool     blinking       = false;
bool     led_state      = false;

}  // namespace

void battery_led_init(void) {
    last_report_us = 0;
    last_toggle_us = 0;
    blinking = false;
    led_state = false;
}

void battery_led_note_report(void) {
    last_report_us = time_us_64();
}

void battery_led_tick(void) {
    const uint64_t now = time_us_64();
    if (last_report_us == 0 || (now - last_report_us) >= REPORT_STALE_US) {
        // No fresh data — bt.cpp owns the LED while disconnected.
        blinking = false;
        return;
    }

    const uint8_t b   = interrupt_in_data[52];
    const uint8_t pct = b & 0x0F;
    const uint8_t st  = (b >> 4) & 0x0F;
    const bool low    = (st == POWER_STATE_DISCHARGING) && (pct <= THRESHOLD_LEVEL);

    if (low) {
        // Critical warning: override disable_pico_led so the user always sees it.
        if (!blinking) {
            blinking = true;
            led_state = true;
            last_toggle_us = now;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
            return;
        }
        if ((now - last_toggle_us) >= BLINK_PERIOD_US) {
            led_state = !led_state;
            last_toggle_us = now;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_state);
        }
    } else if (blinking) {
        blinking = false;
        // Battery recovered or now charging — restore steady-state LED per the user
        // preference flag (LED off when disabled, otherwise the bt.cpp connected = on state).
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, !get_config().disable_pico_led);
    }
}
