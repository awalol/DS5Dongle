//
// Created by awalol on 2026/5/15.
//

#ifndef DS5_BRIDGE_STATE_MGR_H
#define DS5_BRIDGE_STATE_MGR_H
#include <cstdint>

void state_init();
void state_set(uint8_t *data, uint8_t size);
void state_embed_for_audio(uint8_t *data, uint8_t size);
void state_update(const uint8_t *data, uint8_t size);
void set_volume(uint8_t value);
void set_volume(uint8_t speaker, uint8_t headset);

// Speaker USB alt changed: schedule post-game light restore when stream closes.
void state_note_speaker_alt(bool was_active, bool now_active);
void state_post_game_task(void);

#endif //DS5_BRIDGE_STATE_MGR_H
