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
// Suprimeix l'apagat automatic del comandament (per suspensio sostinguda)
// durant 'duration_us'. El WoL ho crida en disparar-se: mentre el PC arrenca
// despres del Wake-on-LAN l'USB queda suspes molts segons i sense aixo el
// firmware apagaria el comandament al cap de 3s (causava la 2a pulsacio de PS).
void wake_suppress_poweroff(uint64_t duration_us);
#else
static inline void wake_init(void) {}
static inline void wake_on_bt_connect(void) {}
static inline void wake_on_bt_input(const uint8_t *, uint16_t) {}
static inline void wake_on_bt_disconnect(void) {}
static inline void wake_task(void) {}
static inline void wake_note_usb_reconnect(void) {}
static inline void wake_suppress_poweroff(uint64_t) {}
#endif

#endif //DS5_BRIDGE_WAKE_H
