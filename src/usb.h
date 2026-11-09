//
// Created by awalol on 2026/3/4.
//

#ifndef DS5_BRIDGE_USB_H
#define DS5_BRIDGE_USB_H

#include <cstdint>
#include "port/port_platform.h"

extern bool usb_keyboard_only;
extern bool usb_reconfiguring;
uint8_t usb_keyboard_instance();
void usb_reconnect(bool keyboard_only);
#if defined(PORT_PLATFORM_ESP32S31)
void usb_reconnect_task(); // main loop: completes a usb_reconnect() once its 150 ms gap has passed
#endif

extern uint8_t mute[2]; // 0: SPEAKER(0x02) 1: MIC(0x05)
extern float volume[2]; // 0: SPEAKER(0x02) 1: MIC(0x05)

#endif //DS5_BRIDGE_USB_H
