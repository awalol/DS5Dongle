//
// Created by awalol on 2026/5/15.
//

#ifndef DS5_BRIDGE_STATE_MGR_H
#define DS5_BRIDGE_STATE_MGR_H
#include <cstdint>

void state_init();
void state_set(uint8_t *data, uint8_t size);
void state_update(const uint8_t *data, uint8_t size);
void set_volume(uint8_t value);
void set_volume(uint8_t speaker, uint8_t headset);
void state_restore_idle_lights(void);

#endif //DS5_BRIDGE_STATE_MGR_H
