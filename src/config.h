//
// Created by awalol on 2026/5/4.
//

#ifndef DS5_BRIDGE_CONFIG_H
#define DS5_BRIDGE_CONFIG_H

#include <cstdint>

struct __attribute__((packed)) Config_body {
    uint8_t config_version; // Config Version
    float haptics_gain; // [1.0,2.0]
    uint8_t speaker_volume; // [0,127] // unused
    uint8_t headset_volume; // [0,127] // max 0x7f // unused
    uint8_t speaker_gain; // [0,7] (0: auto)
    uint8_t inactive_time; // [0,60] min (0: disable)
    uint8_t disable_pico_led; // bool
    uint8_t polling_rate_mode; // 0: 250Hz fixed, 1: 500Hz fixed, 2: real-time, 3: 1000Hz fixed
    uint8_t audio_buffer_length; // [16,128]
    uint8_t controller_mode; // 0: DS5, 1: DSE, 2: Auto
    uint8_t enable_usb_sn; // 0: disable,1: enable
    uint8_t ps_shortcut_enabled; // 0: disabled, 1: enabled (Xbox Game Bar via HID keyboard)
    uint8_t mic_select; // 0: auto, 1: builtin, 2: headphone, 3: disable
    uint8_t speaker_select; // 0: auto, 1: builtin, 2: headphone, 3: disable
    uint8_t enable_wake; // bool: 0 disabled (default), 1 wake host on PS press (USB remote wakeup)
    uint8_t trigger_reduce; // [0,10] (0: auto)
    uint8_t lock_volume; // bool
    uint8_t status_gpio_pin; // board-usable GPIO, 0xff: disabled
    uint8_t status_gpio_mode; // 0: high while connected, 1: button pulse on connect
};

struct __attribute__((packed)) Config {
    uint32_t magic;
    uint32_t crc32; // Config_body crc32, only calc and verify when save
    uint16_t size;  // Config_body size
    Config_body body;
};

static_assert(sizeof(Config_body) == 22);
static_assert(sizeof(Config) == 32);

void config_default();
void config_load();
bool config_save();
Config_body& get_config();
bool battery_feedback_enabled();
void set_battery_feedback_enabled(bool enabled);
uint8_t battery_feedback_save_status(); // 0: unconfirmed, 1: verified, 2: failed
void set_config(const uint8_t *new_config, const uint16_t len);
void config_valid();
void set_config(const Config_body &new_config);
extern bool is_dse;

#endif //DS5_BRIDGE_CONFIG_H
