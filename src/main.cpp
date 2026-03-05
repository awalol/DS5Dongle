//
// Created by awalol on 2026/3/4.
//

#include <cstdio>
#include "bsp/board_api.h"
#include "usb.h"
#include "bt.h"
#include "utils.h"
#include "pico/multicore.h"
#include "resample.h"
#include "audio.h"

int reportSeqCounter = 0;
uint8_t packetCounter = 0;

uint8_t interrupt_in_data[63];

bool interrupt_in_cb(repeating_timer* rt) {
    if (tud_ready()) {
        tud_hid_report(0x01,interrupt_in_data,63);
    }
    return true;
}

void on_bt_data(CHANNEL_TYPE channel, uint8_t *data, uint16_t len) {
    printf("[Main] BT data callback: channel=%u len=%u\n", channel, len);
    if (channel == INTERRUPT && data[1] == 0x31) {
        memcpy(interrupt_in_data,data + 3,63);
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

    uint8_t *feature_data = get_feature_data(report_id, reqlen);
    if (feature_data) {
        memcpy(buffer, feature_data, reqlen);
    }

    return feature_data ? reqlen : 0;
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize) {
    // This example doesn't use multiple report and report ID
    (void) itf;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) bufsize;

    switch (buffer[0]) {
        case 0x02: {
            uint8_t outputData[78];
            outputData[0] = 0x31;
            outputData[1] = reportSeqCounter << 4;
            if (++reportSeqCounter == 256) {
                reportSeqCounter = 0;
            }
            outputData[2] = 0x10;
            memcpy(outputData + 3,buffer + 1,bufsize - 1);
            bt_write(outputData,sizeof(outputData));
            break;
        }
    }
}

int main() {
    board_init();

    tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO
    };
    tusb_init(BOARD_TUD_RHPORT, &dev_init);

    board_init_after_tusb();

    bt_init();
    bt_register_data_callback(on_bt_data);

    multicore_launch_core1(core1_entry);
    repeating_timer rt{};
    add_repeating_timer_ms(4,interrupt_in_cb,nullptr,&rt);

    while (1) {
        tud_task();
    }
}
