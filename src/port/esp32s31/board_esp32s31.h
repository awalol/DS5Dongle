#pragma once

// ESP32-S31-Function-CoreBoard-1 pin map.
//
// Board reference: ESP32-S31-WROOM-3 module, 16MB flash / 16MB PSRAM, USB 2.0
// high-speed OTG, ES8311 codec + NS4150B amplifier, addressable RGB LED,
// RJ45 gigabit Ethernet.
//
// Verified against the Espressif user guide:
// documentation.espressif.com/esp-dev-kits/en/latest/esp32s31/esp32-s31-function-coreboard-1/

// Addressable RGB LED (WS2812-style, driven over RMT).
// User guide: "Addressable RGB LED, driven by GPIO60".
#define BOARD_STATUS_LED_GPIO 60

// BOOT button, active low. GPIO61, NOT GPIO0 -- 61 is the S31's boot-mode
// strapping pin; GPIO0 is an ordinary pin on header J2 with no button, so
// reading it would float.
#define BOARD_BOOT_BUTTON_GPIO 61

// USB: nothing to configure here; see ports/esp32s31/README.md for cabling.
//
// The UTMI PHY's D+/D- go to the Type-A receptacle, which does not force host
// role -- device vs host is a software choice. The real conflict is VBUS: the
// board sources 5V on that port through a TPS2051C whose active-high EN is
// strapped to VCC_5V through a populated 10k and reaches no GPIO, so it cannot
// be turned off. Use a VBUS-disconnected (data-only) A-to-A cable and power the
// board from a Type-C port.
