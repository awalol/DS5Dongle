//
// Created by awalol on 2026/3/5.
//

#ifndef DS5_BRIDGE_AUDIO_H
#define DS5_BRIDGE_AUDIO_H

#include <cstdint>

void audio_init();
void audio_loop();
void core1_entry();
void set_headset(bool state);
uint32_t audio_fifo_drops();
uint32_t opus_fifo_drops();

#endif //DS5_BRIDGE_AUDIO_H