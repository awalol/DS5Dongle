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
void set_plug_mic(bool state);
// Resolves config.mic_select into the MicSelect to send to the controller: the
// configured value when the user set one, otherwise External/Internal Only
// depending on whether a jack mic is present. Always concrete, never Auto.
uint8_t effective_mic_select();
void set_mic_active(bool active);
bool audio_mic_active();
void mic_add_queue(uint8_t *data, uint16_t len);
void update_mic_status();

#endif //DS5_BRIDGE_AUDIO_H