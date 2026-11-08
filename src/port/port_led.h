#pragma once

namespace port {

// Single status LED, driven as on/off.
//
// On Pico2W this is the CYW43 wireless-chip GPIO. On the
// ESP32-S31 boards there is no plain status LED — each has one addressable RGB
// LED on GPIO60 — so the backend drives it over RMT and
// maps "on" to a fixed dim white. Anything wanting colour should extend this
// interface rather than reaching for the strip driver directly.

void led_init();
void led_set(bool on);

} // namespace port
