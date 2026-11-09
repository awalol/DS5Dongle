//
// BOOTSEL button gestures, split out of bt.cpp.
//

#include "port/port.h"
#include "button_functions.h"

#include <cstdio>

#include "bt.h"

// Gesture thresholds, in samples at the 100 ms (10 Hz) poll cadence.
static constexpr int HOLD_SAMPLES = 15;         // ~1.5 s held -> clear all pairings
static constexpr int CLICK_WINDOW_SAMPLES = 5;  // ~500 ms allowed between clicks

// FSM: 0 idle, 1 pressing (counting for hold), 2 held (fired), 3 wait-for-next-click.
static int button_fsm = 0;
static int button_press_samples = 0;
static int button_wait_samples = 0;
static int button_click_count = 0;
static uint32_t button_last_check_ms = 0;

// Act on a completed click sequence once the inter-click window closes.
static void button_dispatch(int clicks) {
    if (clicks <= 1) {
        bt_bootsel_click_action(); // single click -> pair / switch controller
    } else if (clicks == 2) {
        // double click -> normal reboot.
        printf("[BTN] BOOTSEL double click - reboot\n");
        port::reboot(); // noreturn
    } else if (port::has_bootloader_reboot()) {
        // triple click -> reboot into the ROM bootloader for reflashing.
        printf("[BTN] BOOTSEL triple click - reboot to BOOTSEL\n");
        port::reboot_to_bootloader(); // noreturn
    } else {
        // No third gesture on this target: there is no software route to a flash
        // mode, and the host tool straps the chip into download boot itself. Do
        // nothing rather than fall through to a reboot, which would make a
        // mis-counted click drop the controller link.
        printf("[BTN] BOOTSEL triple click - no flash mode on this target; hold "
               "BOOT while resetting, or just run the flash tool\n");
    }
}

// Poll BOOTSEL at 10 Hz and dispatch single / double / triple click + hold:
//   - hold (>= HOLD_SAMPLES, ~1.5 s) -> clear all pairings
//   - 1 click  -> pair / switch        2 clicks -> reboot        3 clicks -> BOOTSEL
// Clicks are counted across the inter-click window; the action fires when it closes.
// Also services the deferred blacklist persist on the same cadence.
void button_check() {
    // No connection gate: safe to poll during audio because port::boot_button_pressed()
    // uses flash_safe_execute(), which parks core1 (the audio core) for the QSPI
    // CSn float -- see flash_safe_execute_core_init() and PICO_FLASH_ASSUME_CORE1_SAFE=0.
    uint32_t now = port::now_ms();
    if (now - button_last_check_ms < 100) return;
    button_last_check_ms = now;

    bt_blacklist_persist_if_dirty();

    bool pressed = port::boot_button_pressed();

    switch (button_fsm) {
        case 0: // IDLE - wait for the first press
            if (pressed) {
                button_fsm = 1;
                button_press_samples = 1;
            }
            break;
        case 1: // PRESSING - counting samples for hold
            if (pressed) {
                if (++button_press_samples >= HOLD_SAMPLES) {
                    button_click_count = 0;
                    button_fsm = 2;
                    bt_bootsel_hold_action();
                }
            } else {
                // released before the hold threshold -> count this click and
                // wait to see whether more clicks follow
                button_click_count++;
                button_fsm = 3;
                button_wait_samples = 0;
            }
            break;
        case 2: // HELD - hold already fired, wait for release
            if (!pressed) {
                button_fsm = 0;
                button_press_samples = 0;
            }
            break;
        case 3: // WAIT - released; watch for the next press inside the window
            if (pressed) {
                button_fsm = 1;
                button_press_samples = 1;
            } else if (++button_wait_samples >= CLICK_WINDOW_SAMPLES) {
                int clicks = button_click_count;
                button_click_count = 0;
                button_fsm = 0;
                button_press_samples = 0;
                button_dispatch(clicks);
            }
            break;
    }
}
