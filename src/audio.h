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
void set_mic_active(bool active);
bool audio_mic_active();
void mic_add_queue(uint8_t *data, uint16_t len);
void update_mic_status();

// Re-push speaker/headset volume + controller audio subscription after BT or USB
// resume (needed when wake suspend cycles power off the controller radio).
void audio_resync_speaker_path(void);

#endif //DS5_BRIDGE_AUDIO_H