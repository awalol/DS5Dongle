#pragma once

// ESP32-S31 board pin map.
//
// Target: Maple ESP32S31 Alef (AnalogLamb / XPU Labs). Also correct for the
// Espressif ESP32-S31-Function-CoreBoard-1, which wires the same ESP32-S31-WROOM-3
// module (16MB flash / 16MB PSRAM) to the same two pins, so one image runs on
// both. A board that differs here needs its own header.
//
// Sources: the Alef schematic excerpts in its user guide (github.com/xpulabs/
// docs_en, maple_esp32/src/maple_esp32s31) and the Espressif user guide,
// documentation.espressif.com/esp-dev-kits/en/latest/esp32s31/esp32-s31-function-coreboard-1/

// Addressable RGB LED (WS2812-style, driven over RMT).
// Alef: D2, an SK6805-2427 on 3.3 V, DIN straight from IO60.
// CoreBoard-1: "Addressable RGB LED, driven by GPIO60", via a non-inverting
// level shifter to 5 V. Neither inverts, so the driver config is shared.
#define BOARD_STATUS_LED_GPIO 60

// BOOT button, active low. GPIO61, NOT GPIO0 -- 61 is the S31's boot-mode
// strapping pin; GPIO0 is an ordinary header pin with no button, so reading it
// would float. Alef: BOOT1 from IO61 to GND.
#define BOARD_BOOT_BUTTON_GPIO 61

// USB: nothing to configure here; see ports/esp32s31/README.md for cabling.
//
// The UTMI PHY's D+/D- reach one connector per board, and device vs host is a
// software choice either way. Alef: the Type-C marked HOST, which also powers
// the board, so one cable to the PC is the whole setup. CoreBoard-1: the Type-A
// receptacle, which sources 5 V through an always-on TPS2051C, so it needs a
// VBUS-disconnected A-to-A cable and power from a Type-C port.
