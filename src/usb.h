//
// Created by awalol on 2026/3/4.
//

#ifndef DS5_BRIDGE_USB_H
#define DS5_BRIDGE_USB_H

extern uint8_t mute[2]; // 0: SPEAKER(0x02) 1: MIC(0x05)
extern float volume[2]; // 0: SPEAKER(0x02) 1: MIC(0x05)

bool usb_is_sleep();

#endif //DS5_BRIDGE_USB_H