//
// Created by awalol on 2026/3/4.
//

#ifndef DS5_BRIDGE_BT_H
#define DS5_BRIDGE_BT_H

#include <cstdint>
#include <vector>

enum CHANNEL_TYPE {
    INTERRUPT,
    CONTROL
};

typedef void (*bt_data_callback_t)(CHANNEL_TYPE channel, uint8_t *data, uint16_t len);

struct SetStateData;

extern const uint8_t state_init_data[66];

int bt_init();
void bt_register_data_callback(bt_data_callback_t callback);
void bt_send_packet(uint8_t *data, uint16_t len);
void bt_send_control(uint8_t *data, uint16_t len);
bool bt_disconnect();
bool bt_is_connected();
void bt_set_scan_idle();
void bt_set_scan_active();
// Periodic hook; call from the main loop alongside the other *_task()s.
void bt_task();
void bt_write(const uint8_t *data, uint16_t len);
void bt_get_signal_strength(int8_t *rssi);
std::vector<uint8_t> get_feature_data(uint8_t reportId, uint16_t len);

// Issue a GET_REPORT (0x43 <id>) over the HID control channel and pump the BT
// stack until the matching 0xA3 response arrives, or until timeout_ms expires.
// Returns the cached/just-received report body (without the report id byte).
// Must be called from the main context (e.g. the USB GET_REPORT callback): it
// drives port::bt_transport_poll() itself and never calls tud_task().
std::vector<uint8_t> get_feature_data_live(uint8_t reportId, uint16_t timeout_ms);

void init_feature();

// Remember what the host last asked the lightbar to be, so the lightbar
// handover can re-apply that rather than overriding it with the firmware default.
void bt_note_host_led(uint8_t r, uint8_t g, uint8_t b);
// Likewise for the player indicators: PlayerLight1..5 in bits 0..4,
// PlayerLightFade in bit 5.
void bt_note_host_player(uint8_t bits);
void set_feature_data(uint8_t reportId, uint8_t* data,uint16_t len);
void bt_inquiring_led();
// BOOTSEL button actions, dispatched from button_functions.cpp.
void bt_bootsel_click_action();
void bt_bootsel_hold_action();
void bt_blacklist_persist_if_dirty();
void update_state(const SetStateData& state);

#endif //DS5_BRIDGE_BT_H
