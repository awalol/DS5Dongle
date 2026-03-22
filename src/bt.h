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
void bt_write(uint8_t* data, uint16_t len);

#define BT_WRITE_PACKET_HEAD 0xA2
// 零拷贝接口：调用方在 vector 中直接构建含 BT_WRITE_PACKET_HEAD 前缀的完整包，
// 通过 move 语义转移所有权，函数内部仅做 checksum 计算，无额外 memcpy。
void bt_write(std::vector<uint8_t>&& packet);
std::vector<uint8_t> get_feature_data(uint8_t reportId, uint16_t len);
void init_feature();

#endif //DS5_BRIDGE_BT_H