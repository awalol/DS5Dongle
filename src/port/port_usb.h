#pragma once

#include <cstdint>

// The bits of USB bring-up that are board-provided rather than TinyUSB-generic.
//
// On Pico these come from TinyUSB's example BSP (bsp/board_api.h), which ships
// inside the pico-sdk. ESP-IDF has no equivalent BSP, so the ESP32-S31 backend
// supplies them directly.

namespace port {

// Root-hub port the device stack runs on.
extern const uint8_t kUsbRhPort;

// Bus speed to request in tusb_rhport_init_t.
//
// RP2350 is full-speed only. ESP32-S31 has a high-speed OTG with a UTMI PHY and
// no FS/LS PHY at all (soc_caps: SOC_USB_UTMI_PHY_NUM 1, SOC_USB_FSLS_PHY_NUM 0),
// so the descriptors' isochronous sizing assumptions need revisiting there --
// see ports/esp32s31/README.md.
extern const uint8_t kUsbSpeed;

// Board init before tusb_init(), and the follow-up some BSPs need afterwards.
void board_init();
void board_init_after_tusb();

// Fill `desc_str` with up to `max_chars` UTF-16 characters of a unique serial
// number; returns the number of characters written.
size_t usb_get_serial(uint16_t *desc_str, size_t max_chars);

} // namespace port
