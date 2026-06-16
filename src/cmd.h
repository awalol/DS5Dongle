//
// Created by awalol on 2026/5/4.
//

#ifndef DS5_BRIDGE_CMD_H
#define DS5_BRIDGE_CMD_H

#include <stdint.h>

bool is_pico_cmd(uint8_t report_id);
uint16_t pico_cmd_get(uint8_t report_id, uint8_t *buffer, uint16_t reqlen);
void pico_cmd_set(uint8_t report_id, uint8_t const *buffer, uint16_t bufsize);
void pico_cmd_legacy_set(uint8_t cmd_id, uint8_t const *buffer, uint16_t bufsize);

#endif //DS5_BRIDGE_CMD_H
