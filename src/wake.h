//
// Created by awalol on 2026/4/30.
//

#ifndef DS5_BRIDGE_WAKE_H
#define DS5_BRIDGE_WAKE_H

#include <cstdint>

#ifdef ENABLE_WAKE_HID
void wake_init(void);
void wake_on_bt_connect(void);
void wake_on_bt_input(const uint8_t *hid_input, uint16_t len);
void wake_on_bt_disconnect(void);
void wake_task(void);
void wake_note_usb_reconnect(void);
// Suppress the controller's automatic power-off (for sustained suspend)
// for 'duration_us'. WoL calls this when triggered: while the PC boots
// after Wake-on-LAN the USB stays suspended for many seconds, and without this
// the firmware would power off the controller after 3s (which caused the 2nd PS press).
void wake_suppress_poweroff(uint64_t duration_us);
// Cancel an armed power-off suppression. WoL calls this if the observation phase
// finds the PC was actually on (so no magic packet is sent): the suppression armed
// on connect must not linger, or a later genuine sleep would skip the controller's
// battery power-off for up to the suppression window.
void wake_cancel_poweroff_suppress(void);
#else
static inline void wake_init(void) {}
static inline void wake_on_bt_connect(void) {}
static inline void wake_on_bt_input(const uint8_t *, uint16_t) {}
static inline void wake_on_bt_disconnect(void) {}
static inline void wake_task(void) {}
static inline void wake_note_usb_reconnect(void) {}
static inline void wake_suppress_poweroff(uint64_t) {}
static inline void wake_cancel_poweroff_suppress(void) {}
#endif

#endif //DS5_BRIDGE_WAKE_H
