//
// Created by awalol on 2026/5/4.
//

#include "cmd.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "bt.h"
#include "config.h"
#include "device/usbd.h"
#include "pico/time.h"
#include "pico/bootrom.h"
#include "audio.h"

// spk_active (main.cpp) + audio_mic_active() (audio.cpp) are surfaced in the
// 0xf9 feature report so the config UI can display the real gated mic/speaker
// state, reflecting the disable_mic / disable_speaker settings.
extern bool spk_active;
extern std::unordered_map<uint8_t, std::vector<uint8_t> > feature_data;

bool is_pico_cmd(uint8_t report_id) {
    return report_id == 0xf6 ||
           report_id == 0xf7 ||
           report_id == 0xf8 ||
           report_id == 0xf9;
}

uint16_t pico_cmd_get(uint8_t report_id, uint8_t *buffer, uint16_t reqlen) {
    if (report_id == 0xf7) {
        printf("[HID] Receive 0xf7 getting config\n");
        if (sizeof(Config_body) > reqlen) {
            printf("[Config] Warning: Config_body overflow\n");
        }
        const auto len = std::min(sizeof(Config_body), static_cast<size_t>(reqlen));
        memcpy(buffer, &get_config(), len);
        return len;
    }
    if (report_id == 0xf8) {
        printf("[HID] Receive 0xf8 getting firmware version\n");
        const auto len = std::min(strlen(PICO_PROGRAM_VERSION_STRING), static_cast<size_t>(reqlen));
        memcpy(buffer, PICO_PROGRAM_VERSION_STRING, len);
        return len;
    }
    if (report_id == 0xf9) {
        int8_t rssi = 0;
        bt_get_signal_strength(&rssi);
        if (reqlen == 0) {
            return 0;
        }
        buffer[0] = rssi;
        if (reqlen >= 2) {
            uint8_t flags = 0x80;
            if (audio_mic_active() && !get_config().disable_mic) flags |= 0x01;
            if (spk_active && !get_config().disable_speaker) flags |= 0x02;
            buffer[1] = flags;
            return 2;
        }
        return 1;
    }
    return 0;
}

void pico_cmd_set(uint8_t report_id, uint8_t const *buffer, uint16_t bufsize) {
    (void) report_id;
    if (bufsize == 0) {
        return;
    }

    if (buffer[0] == 0x01) {
#if ENABLE_VERBOSE
        printf("[CMD] Enter config set func\n");
#endif
        set_config(buffer + 1, bufsize - 1);
    }
    if (buffer[0] == 0x02) {
        printf("[CMD] Enter config save func\n");
        if (config_save()) {
            config_schedule_usb_reconnect_if_layout_changed();
        }
    }
    if (buffer[0] == 0x03) {
        printf("[CMD] Enter tud reconnect func\n");
        tud_disconnect();
        sleep_ms(150);
        tud_connect();
    }
    if (buffer[0] == 0x04) {
        printf("[CMD] Reboot to BOOTSEL (USB bootloader)\n");
        reset_usb_boot(0, 0);
    }
}

template<typename T>
static bool read_config_value(T &value, uint8_t const *buffer, uint16_t bufsize) {
    if (bufsize < sizeof(T)) {
        return false;
    }
    memcpy(&value, buffer, sizeof(T));
    return true;
}

template<typename T>
static bool write_config_value(uint8_t *buffer, uint16_t bufsize, T value) {
    if (bufsize < sizeof(T)) {
        return false;
    }
    memcpy(buffer, &value, sizeof(T));
    return true;
}

static bool set_config_field(uint8_t field_id, uint8_t const *buffer, uint16_t bufsize) {
    Config_body new_config = get_config();

    switch (field_id) {
        case 0x01: {
            float value{};
            if (!read_config_value(value, buffer, bufsize)) return false;
            new_config.haptics_gain = value;
            break;
        }
        case 0x02: {
            uint8_t value{};
            if (!read_config_value(value, buffer, bufsize)) return false;
            new_config.speaker_volume = value;
            break;
        }
        case 0x03: {
            uint8_t value{};
            if (!read_config_value(value, buffer, bufsize)) return false;
            new_config.headset_volume = value;
            break;
        }
        case 0x04: {
            uint8_t value{};
            if (!read_config_value(value, buffer, bufsize)) return false;
            new_config.sync_spk_headset_volume = value;
            break;
        }
        case 0x05: {
            uint8_t value{};
            if (!read_config_value(value, buffer, bufsize)) return false;
            new_config.speaker_gain = value;
            break;
        }
        case 0x06: {
            uint8_t value{};
            if (!read_config_value(value, buffer, bufsize)) return false;
            new_config.inactive_time = value;
            break;
        }
        case 0x07: {
            uint8_t value{};
            if (!read_config_value(value, buffer, bufsize)) return false;
            new_config.disable_inactive_disconnect = value;
            break;
        }
        case 0x08: {
            uint8_t value{};
            if (!read_config_value(value, buffer, bufsize)) return false;
            new_config.disable_pico_led = value;
            break;
        }
        case 0x09: {
            uint8_t value{};
            if (!read_config_value(value, buffer, bufsize)) return false;
            new_config.polling_rate_mode = value;
            break;
        }
        case 0x0a: {
            uint8_t value{};
            if (!read_config_value(value, buffer, bufsize)) return false;
            new_config.audio_buffer_length = value;
            break;
        }
        case 0x0b: {
            uint8_t value{};
            if (!read_config_value(value, buffer, bufsize)) return false;
            new_config.controller_mode = value;
            break;
        }
        case 0x0c: {
            uint8_t value{};
            if (!read_config_value(value, buffer, bufsize)) return false;
            new_config.lock_volume = value;
            break;
        }
        case 0x0d: {
            uint8_t value{};
            if (!read_config_value(value, buffer, bufsize)) return false;
            new_config.disable_usb_sn = value;
            break;
        }
        case 0x0e: {
            uint8_t value{};
            if (!read_config_value(value, buffer, bufsize)) return false;
            new_config.ps_shortcut_enabled = value;
            break;
        }
        case 0x0f: {
            uint8_t value{};
            if (!read_config_value(value, buffer, bufsize)) return false;
            new_config.disable_mic = value;
            break;
        }
        case 0x10: {
            uint8_t value{};
            if (!read_config_value(value, buffer, bufsize)) return false;
            new_config.disable_speaker = value;
            break;
        }
        case 0x11: {
            uint8_t value{};
            if (!read_config_value(value, buffer, bufsize)) return false;
            new_config.enable_wake = value;
            break;
        }
        default:
            printf("[CMD] Unknown config field id: 0x%02X\n", field_id);
            return false;
    }

    set_config(reinterpret_cast<const uint8_t *>(&new_config), sizeof(new_config));
    return true;
}

static bool get_config_field(uint8_t field_id, uint8_t *buffer, uint16_t bufsize) {
    const Config_body &config = get_config();

    switch (field_id) {
        case 0x00:
            return write_config_value(buffer, bufsize, config.config_version);
        case 0x01:
            return write_config_value(buffer, bufsize, config.haptics_gain);
        case 0x02:
            return write_config_value(buffer, bufsize, config.speaker_volume);
        case 0x03:
            return write_config_value(buffer, bufsize, config.headset_volume);
        case 0x04:
            return write_config_value(buffer, bufsize, config.sync_spk_headset_volume);
        case 0x05:
            return write_config_value(buffer, bufsize, config.speaker_gain);
        case 0x06:
            return write_config_value(buffer, bufsize, config.inactive_time);
        case 0x07:
            return write_config_value(buffer, bufsize, config.disable_inactive_disconnect);
        case 0x08:
            return write_config_value(buffer, bufsize, config.disable_pico_led);
        case 0x09:
            return write_config_value(buffer, bufsize, config.polling_rate_mode);
        case 0x0a:
            return write_config_value(buffer, bufsize, config.audio_buffer_length);
        case 0x0b:
            return write_config_value(buffer, bufsize, config.controller_mode);
        case 0x0c:
            return write_config_value(buffer, bufsize, config.lock_volume);
        case 0x0d:
            return write_config_value(buffer, bufsize, config.disable_usb_sn);
        case 0x0e:
            return write_config_value(buffer, bufsize, config.ps_shortcut_enabled);
        case 0x0f:
            return write_config_value(buffer, bufsize, config.disable_mic);
        case 0x10:
            return write_config_value(buffer, bufsize, config.disable_speaker);
        case 0x11:
            return write_config_value(buffer, bufsize, config.enable_wake);
        default:
            printf("[CMD] Unknown config field id: 0x%02X\n", field_id);
            return false;
    }
}

void pico_cmd_legacy_set(uint8_t cmd_id, uint8_t const *buffer, uint16_t bufsize) {
    switch (cmd_id) {
        case 0x01: {
#if ENABLE_VERBOSE
            printf("[CMD] Enter legacy config set func\n");
#endif
            bool success = false;
            if (bufsize < 1) {
                printf("[CMD] Config set missing field id\n");
            } else {
                const uint8_t field_id = buffer[0];
                success = set_config_field(field_id, buffer + 1, bufsize - 1);
                if (!success) {
                    printf("[CMD] Config set failed, field id: 0x%02X\n", field_id);
                }
            }
            uint8_t buf[63]{};
            buf[0] = 0x66;
            buf[1] = 0x01;
            buf[2] = success ? 0x00 : 0x01;
            feature_data[0x81].assign(buf, buf + sizeof(buf));
            break;
        }
        case 0x02: {
            printf("[CMD] Enter config save func\n");
            config_save();
            break;
        }
        case 0x03: {
            printf("[CMD] Enter tud reconnect func\n");
            tud_disconnect();
            sleep_ms(150);
            tud_connect();
            break;
        }
        case 0x04: {
            printf("[CMD] get config field\n");
            uint8_t buf[63]{};
            buf[0] = 0x66;
            buf[1] = 0x04;
            if (bufsize < 1) {
                printf("[CMD] Config get missing field id\n");
                buf[2] = 0xff;
            } else {
                const uint8_t field_id = buffer[0];
                buf[2] = field_id;
                if (!get_config_field(field_id, buf + 3, sizeof(buf) - 3)) {
                    printf("[CMD] Config get failed, field id: 0x%02X\n", field_id);
                }
            }
            feature_data[0x81].assign(buf, buf + sizeof(buf));
            break;
        }
        case 0x05: {
            printf("[CMD] get firmware version\n");
            uint8_t buf[63]{};
            buf[0] = 0x66;
            buf[1] = 0x05;
            const auto len = std::min(strlen(PICO_PROGRAM_VERSION_STRING), sizeof(buf) - 2);
            memcpy(buf + 2, PICO_PROGRAM_VERSION_STRING, len);
            feature_data[0x81].assign(buf, buf + sizeof(buf));
            break;
        }
        case 0x06: {
            printf("[CMD] get signal strength\n");
            uint8_t buf[63]{};
            buf[0] = 0x66;
            buf[1] = 0x06;
            int8_t rssi = 0;
            bt_get_signal_strength(&rssi);
            buf[2] = rssi;
            uint8_t flags = 0x80;
            if (audio_mic_active() && !get_config().disable_mic) flags |= 0x01;
            if (spk_active && !get_config().disable_speaker) flags |= 0x02;
            buf[3] = flags;
            feature_data[0x81].assign(buf, buf + sizeof(buf));
            break;
        }
    }
}
