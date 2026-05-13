//
// Low-battery LED indicator for the Pico onboard LED.
// Reads PowerPercent / PowerState from interrupt_in_data[52]
// (DualSense BT 0x31 report, see USBGetStateData in utils.h).
//

#pragma once

void battery_led_init(void);

// Call once per main-loop iteration. Drives the LED blink while the
// battery is low and the controller is connected; otherwise no-op.
void battery_led_tick(void);

// Call from the BT input-report callback whenever a fresh 0x31 report
// has been copied into interrupt_in_data. Used to detect disconnection
// via stale-report timeout.
void battery_led_note_report(void);
