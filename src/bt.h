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

int bt_init();
void bt_register_data_callback(bt_data_callback_t callback);
void bt_send_packet(uint8_t *data, uint16_t len);
void bt_send_control(uint8_t *data, uint16_t len);
// Returns true when the power-off command was actually queued to the controller
// (or there is no controller connected, so there is nothing to power off).
// Returns false when the send was dropped (e.g. ACL buffer busy) — retry later.
bool bt_power_off_controller();
bool bt_disconnect();
bool bt_is_connected();
void bt_set_scan_idle();
void bt_set_scan_active();
void dse_unlock_task();
bool bt_dse_profiles_ready();
void bt_write(const uint8_t *data, uint16_t len);
void bt_get_signal_strength(int8_t *rssi);
std::vector<uint8_t> get_feature_data(uint8_t reportId,uint16_t len);
void init_feature();
// Returns true when the SET report was queued on the control channel; false when
// there is no control channel or l2cap dropped the send (ACL buffer busy).
bool set_feature_data(uint8_t reportId, uint8_t* data,uint16_t len);
void bt_inquiring_led();
// BOOTSEL button actions, dispatched from button_functions.cpp.
void bt_bootsel_click_action();
void bt_bootsel_hold_action();
void bt_blacklist_persist_if_dirty();

#endif //DS5_BRIDGE_BT_H
