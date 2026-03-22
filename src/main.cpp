//
// Created by awalol on 2026/3/4.
//

#include <cstdio>
#include "bsp/board_api.h"
#include "bt.h"
#include "utils.h"
#include "resample.h"
#include "audio.h"
#include "pico/cyw43_arch.h"
// #include "pico/stdlib.h"
#include <vector>

int reportSeqCounter = 0;
uint8_t packetCounter = 0;

uint8_t interrupt_in_data[63] = {
    0x7f, 0x7d, 0x7f, 0x7e, 0x00, 0x00, 0xa7,
    0x08, 0x00, 0x00, 0x00, 0x52, 0x43, 0x30, 0x41,
    0x01, 0x00, 0x0e, 0x00, 0xef, 0xff, 0x03, 0x03,
    0x7b, 0x1b, 0x18, 0xf0, 0xcc, 0x9c, 0x60, 0x00,
    0xfc, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x09, 0x09, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xa7, 0xad, 0x60, 0x00, 0x29, 0x18, 0x00,
    0x53, 0x9f, 0x28, 0x35, 0xa5, 0xa8, 0x0c, 0x8b
};

void interrupt_loop() {
    if (!tud_hid_ready()) return;
    if (!tud_hid_report(0x01, interrupt_in_data, 63)) {
        printf("[USBHID] tud_hid_report error\n");
    }
}

void on_bt_data(CHANNEL_TYPE channel, uint8_t *data, uint16_t len) {
    // printf("[Main] BT data callback: channel=%u len=%u\n", channel, len);
    if (channel == INTERRUPT && data[1] == 0x31) {
        memcpy(interrupt_in_data, data + 3, 63);
    }
}

// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request
uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen) {
    (void) itf;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) reqlen;

    std::vector<uint8_t> feature_data = get_feature_data(report_id, reqlen);
    if (!feature_data.empty()) {
        memcpy(buffer, feature_data.data() + 1, feature_data.size() - 1);
    }

    return feature_data.empty() ? 0 : feature_data.size() - 1;
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize) {
    (void) itf;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) bufsize;

    switch (buffer[0]) {
        case 0x02: {
            constexpr auto PACKET_SIZE = 78;
            if (bufsize - 1 > PACKET_SIZE - 4) { // 4字节报文头
                break;
            }

            // uint8_t outputData[78];
            std::vector<uint8_t> pkt;
            pkt.assign(1 + PACKET_SIZE, 0);
            pkt[0] = BT_WRITE_PACKET_HEAD;
            pkt[1] = 0x31;
            pkt[2] = reportSeqCounter << 4;
            if (++reportSeqCounter == 256) {
                reportSeqCounter = 0;
            }
            pkt[3] = 0x10;
            
            memcpy(pkt.data() + 4, buffer + 1, bufsize - 1);
            bt_write(std::move(pkt));
            break;
        }
    }
}

int main() {
    // stdio_init_all();
    board_init();

    tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO
    };
    tusb_init(BOARD_TUD_RHPORT, &dev_init);

    board_init_after_tusb();

    bt_init();
    bt_register_data_callback(on_bt_data);

    audio_init();

    while (1) {
        cyw43_arch_poll();
        tud_task();
        audio_loop();
        interrupt_loop();
    }
}
