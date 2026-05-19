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

// Call from the BT disconnect handler. Cancels any in-progress blink,
// forces the LED off, and arms the module so it ignores the cached
// (now-stale) battery byte until a fresh report arrives on the next
// connection. Without this, the LED can stay frozen in whichever state
// it was at the moment of disconnect, or briefly resume blinking during
// reconnect retries while interrupt_in_data[52] still reads low.
void battery_led_on_disconnect(void);
