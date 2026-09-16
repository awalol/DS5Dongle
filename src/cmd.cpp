//
// Created by awalol on 2026/5/4.
//

#include "port/port.h"
#include "cmd.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "bt.h"
#include "config.h"
#include "device/usbd.h"
#include "audio.h"
#include "wake.h"

extern bool spk_active;

bool is_pico_cmd(uint8_t report_id) {
    if (report_id == 0xf6 ||
        report_id == 0xf7 ||
        report_id == 0xf8 ||
        report_id == 0xf9
    ) {
        return true;
    }
    return false;
}

uint16_t pico_cmd_get(uint8_t report_id, uint8_t *buffer, uint16_t reqlen) {
    if (report_id == 0xf7) {
        printf("[HID] Receive 0xf7 getting config\n");
        if (sizeof(Config_body) > reqlen) {
            printf("[Config] Warning: Config_body overflow\n");
        }
        const auto len = std::min(sizeof(Config_body),static_cast<size_t>(reqlen));
        memcpy(buffer,&get_config(),len);
        return len;
    }
    if (report_id == 0xf8) {
        printf("[HID] Receive 0xf8 getting firmware version\n");
        const auto len = std::min(strlen(DS5_FIRMWARE_VERSION), static_cast<size_t>(reqlen));
        memcpy(buffer, DS5_FIRMWARE_VERSION, len);
        return len;
    }
    if (report_id == 0xf9) {
        // [-128,0]
        int8_t rssi = 0;
        bt_get_signal_strength(&rssi);
        if (reqlen == 0) {
            return 0;
        }
        buffer[0] = rssi;
        // byte 1: real audio gating state, for the config UI to display.
        //   bit7 = valid marker (firmware without this byte leaves it 0)
        //   bit0 = controller mic actually streaming (host opened it AND mic_select != disable)
        //   bit1 = controller speaker actually driven (host opened it AND speaker_select != disable)
        if (reqlen >= 2) {
            uint8_t flags = 0x80;
            if (audio_mic_active() && get_config().mic_select != 3) flags |= 0x01;
            if (spk_active && get_config().speaker_select != 3) flags |= 0x02;
            buffer[1] = flags;
            return 2;
        }
#if ENABLE_VERBOSE
        printf("[HID] 0xf9 RSSI=%d raw=0x%02X\n", rssi, buffer[0]);
#endif
        return 1;
    }
    return 0;
}

void pico_cmd_set(uint8_t report_id, uint8_t const *buffer, uint16_t bufsize) {
    (void) report_id;
    if (bufsize == 0) {
        return;
    }

    // 0x01 update config in variable
    // 0x02 write config to flash
    // 0x03 reconnect tinyusb device;
    if (buffer[0] == 0x01) {
#if ENABLE_VERBOSE
        printf("[CMD] Enter config set func\n");
#endif
        set_config(buffer + 1, bufsize - 1);
    }
    if (buffer[0] == 0x02) {
        printf("[CMD] Enter config save func\n");
        config_save();
    }
    if (buffer[0] == 0x03) {
        printf("[CMD] Enter tud reconnect func\n");
        wake_note_usb_reconnect();   // this disconnect is intentional, not a host sleep
        tud_disconnect();
        port::delay_ms(150);
        tud_connect();
    }
}
