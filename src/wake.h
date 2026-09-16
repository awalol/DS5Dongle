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
// True while the wake sequence owns the shared keyboard endpoint.
bool wake_keyboard_busy();
void wake_note_usb_reconnect(void);
#else
static inline void wake_init(void) {}
static inline void wake_on_bt_connect(void) {}
static inline void wake_on_bt_input(const uint8_t *, uint16_t) {}
static inline void wake_on_bt_disconnect(void) {}
static inline void wake_task(void) {}
static inline bool wake_keyboard_busy() { return false; }
static inline void wake_note_usb_reconnect(void) {}
#endif

#endif //DS5_BRIDGE_WAKE_H
