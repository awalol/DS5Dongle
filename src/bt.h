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

int bt_init();
void bt_register_data_callback(bt_data_callback_t callback);
void bt_send_packet(uint8_t *data, uint16_t len);
void bt_send_control(uint8_t *data, uint16_t len);
bool bt_disconnect();
bool bt_is_connected();
void bt_set_scan_idle();
void bt_set_scan_active();
void dse_unlock_task();
bool bt_dse_profiles_ready();
// Periodic hook; call from the main loop alongside the other *_task()s.
void bt_task();

void bt_write(const uint8_t *data, uint16_t len);
void bt_get_signal_strength(int8_t *rssi);
std::vector<uint8_t> get_feature_data(uint8_t reportId,uint16_t len);
void init_feature();

// Remember what the host last asked the lightbar to be, so the ResetLights
// pulse can re-apply that rather than overriding it with the firmware default.
void bt_note_host_led(uint8_t r, uint8_t g, uint8_t b);
void set_feature_data(uint8_t reportId, uint8_t* data,uint16_t len);
void bt_inquiring_led();
// BOOTSEL button actions, dispatched from button_functions.cpp.
void bt_bootsel_click_action();
void bt_bootsel_hold_action();
void bt_blacklist_persist_if_dirty();
void update_state(const SetStateData& state);

#endif //DS5_BRIDGE_BT_H
