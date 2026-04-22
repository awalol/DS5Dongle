//
// Created by awalol on 2026/3/5.
//

#ifndef DS5_BRIDGE_AUDIO_H
#define DS5_BRIDGE_AUDIO_H
#include <cstdint>

void audio_init();
void audio_loop();
void send_speaker(const uint8_t *data);
void send_combine(const uint8_t* speaker,const uint8_t* haptics);
void send_haptics(const int8_t* data);
void vendor_loop();

#endif //DS5_BRIDGE_AUDIO_H