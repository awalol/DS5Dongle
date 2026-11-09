#pragma once

#include <cstdint>

// The user-configurable status GPIO (see src/status_gpio.cpp), which drives an
// external LED or acts as a momentary button output. Pin numbering is the
// platform's own -- config stores a raw pin number, so a config written on a
// Pico does not carry over to an ESP32-S31 board.

namespace port {

// Number of GPIO pins the platform exposes; status_gpio validates against this.
extern const uint32_t kNumGpioPins;

// Configure `pin` as a push-pull output, driven low.
void gpio_init_output(uint32_t pin);

void gpio_write(uint32_t pin, bool level);

} // namespace port
