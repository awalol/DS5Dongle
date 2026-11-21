#pragma once

#include <cstdint>

struct USBGetStateData;

// touchpad_mouse config values.
enum TouchpadMouseMode : uint8_t {
    TouchpadMouseOff = 0,
    TouchpadMouseShared = 1,    // Touchpad moves the cursor and still reaches the gamepad report
    TouchpadMouseExclusive = 2, // While the mouse is active, the gamepad sees an idle touchpad
};

constexpr uint8_t TOUCHPAD_MOUSE_SPEED_MIN = 1;
constexpr uint8_t TOUCHPAD_MOUSE_SPEED_MAX = 20;
constexpr uint8_t TOUCHPAD_MOUSE_SPEED_DEFAULT = 10;
// Report ID of the mouse collection inside the consumer-control HID interface.
constexpr uint8_t TOUCHPAD_MOUSE_REPORT_ID = 3;

// Set by the configuration descriptor callback: true when the last enumeration
// exposed the mouse collection, so reports have somewhere to go.
extern bool touchpad_mouse_enumerated;

#ifdef ENABLE_WAKE_HID
void touchpad_mouse_tick(const USBGetStateData &state);
void touchpad_mouse_task();
void touchpad_mouse_filter_report(USBGetStateData &report);
#else
static inline void touchpad_mouse_tick(const USBGetStateData &) {}
static inline void touchpad_mouse_task() {}
static inline void touchpad_mouse_filter_report(USBGetStateData &) {}
#endif
