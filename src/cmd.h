//
// Created by awalol on 2026/5/4.
//

#ifndef DS5_BRIDGE_CMD_H
#define DS5_BRIDGE_CMD_H

#include <stdint.h>

// 0xF6 funcids carried in buffer[0] after the report id.
enum : uint8_t {
    PICO_CMD_UPDATE_CONFIG = 0x01,
    PICO_CMD_SAVE_TO_FLASH = 0x02,
    PICO_CMD_RECONNECT_USB = 0x03,
    PICO_CMD_REBOOT_BOOTSEL = 0x04, // reboot into the ROM USB bootloader
    PICO_CMD_SHORTCUT_LEARNING = 0x05, // buffer[1] != 0 starts the 5s learning window
};

void pico_cmd_set(uint8_t report_id, uint8_t const *buffer,uint16_t bufsize);
bool is_pico_cmd(uint8_t report_id);
uint16_t pico_cmd_get(uint8_t report_id, uint8_t *buffer, uint16_t reqlen);

#endif //DS5_BRIDGE_CMD_H
