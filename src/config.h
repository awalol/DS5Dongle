//
// Created by awalol on 2026/5/4.
//

#ifndef DS5_BRIDGE_CONFIG_H
#define DS5_BRIDGE_CONFIG_H

#include <cstddef>
#include <cstdint>

#include "button_remap.h"
#include "button_shortcut.h"
#include "usb_descriptors.h"

constexpr uint16_t CONFIG_STORAGE_SIZE = 64;

//    64 bytes    |
//     Config     |     Button

struct __attribute__((packed)) Config_body {
    uint8_t config_version; // Config Version
    float haptics_gain; // [1.0,2.0]
    uint8_t speaker_volume; // [0,127] // unused
    uint8_t headset_volume; // [0,127] // max 0x7f // unused
    uint8_t speaker_gain; // [0,7] (0: auto)
    uint8_t inactive_time; // [0,60] min (0: disable)
    uint8_t disable_pico_led; // bool
    uint8_t polling_rate_mode; // 0: 250Hz, 1: 500Hz, 2: 1000Hz, 3: Real-Time
    uint8_t audio_buffer_length; // [32,128]
    uint8_t controller_mode; // 0: DS5, 1: DSE, 2: Auto
    uint8_t enable_usb_sn; // 0: disable,1: enable
    uint8_t enable_keyboard; // bool: expose the USB keyboard interface
    uint8_t mic_select; // 0: auto, 1: builtin, 2: headphone, 3: disable
    uint8_t speaker_select; // 0: auto, 1: builtin, 2: headphone, 3: disable
    uint8_t enable_wake; // bool: 0 disabled (default), 1 wake host on PS press (USB remote wakeup)
    uint8_t trigger_reduce; // [0,10] (0: auto)
    uint8_t lock_volume; // bool
    uint8_t status_gpio_pin; // board-usable GPIO, 0xff: disabled
    uint8_t status_gpio_mode; // 0: high while connected, 1: button pulse on connect
    uint8_t packet_mode; // 0: double packet(default), 1: single packet
    uint8_t disconnect_on_dock; // bool: 0 disable(default), 1 enable
};

static_assert(sizeof(Config_body) + 1 <= 63); // 0xF6 funcid + body

struct __attribute__((packed)) Config {
    uint32_t magic;
    uint32_t crc32; // ConfigStorage payload crc32, only calc and verify when save
    uint16_t size;  // Config_body size
    Config_body body;
    uint8_t reserved[CONFIG_STORAGE_SIZE - 10 - sizeof(Config_body)];
};

struct __attribute__((packed)) Button {
    uint8_t button_remap[BUTTON_REMAP_COUNT]; // orig_btn:target_btn
    ButtonShortcut shortcuts[BUTTON_SHORTCUT_COUNT];
};

struct __attribute__((packed)) ConfigStorage {
    Config config;
    Button button;
};

static_assert(offsetof(ConfigStorage, button) == CONFIG_STORAGE_SIZE);
static_assert(BUTTON_REMAP_COUNT <= BUTTON_REPORT_SIZE); // 0xFA payload
static_assert(sizeof(Button::shortcuts) <= BUTTON_REPORT_SIZE); // 0xFB payload

void config_default();
void config_load();
bool config_save();
Config_body& get_config();
Button& get_button();
void set_config(const uint8_t *new_config, const uint16_t len);
void set_button_remap(const uint8_t *new_remap, uint16_t len);
void set_shortcut(const uint8_t *new_shortcuts, uint16_t len);
void config_valid();
void button_remap_valid();
void shortcut_valid();
void set_config(const Config_body &new_config);
extern bool is_dse;

#endif //DS5_BRIDGE_CONFIG_H
