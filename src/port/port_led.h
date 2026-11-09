#pragma once

#include <cstdint>

namespace port {

// Single status LED, driven as on/off.
//
// On Pico2W this is the CYW43 wireless-chip GPIO. On the
// ESP32-S31 boards there is no plain status LED — each has one addressable RGB
// LED on GPIO60 — so the backend drives it over RMT and
// maps "on" to a dim colour: white by default, or whatever led_set_on_color()
// last chose. Anything wanting colour should extend this interface rather than
// reaching for the strip driver directly.

void led_init();
void led_set(bool on);

// The colour "on" shows, as a full-scale RGB value the backend dims to suit its
// LED; applied at once if the LED is lit. led_reset_on_color() restores the
// default. Single-colour LEDs (Pico2W) ignore both.
void led_set_on_color(uint8_t r, uint8_t g, uint8_t b);
void led_reset_on_color();

} // namespace port
