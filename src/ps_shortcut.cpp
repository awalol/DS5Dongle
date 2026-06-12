#include "ps_shortcut.h"
#include "config.h"
#include "tusb.h"
#include "class/hid/hid.h"
#include "pico/time.h"

#define PS_KBD_INSTANCE 1

static bool ps_was_pressed       = false;
static uint32_t ps_press_time    = 0;
static bool long_press_fired     = false;
static bool key_release_pending  = false;
static uint32_t key_release_time = 0;
static uint32_t last_high_time   = 0;
static bool is_ps_pressed        = false;

void ps_shortcut_reset() {
    if (key_release_pending && tud_hid_n_ready(PS_KBD_INSTANCE)) {
        tud_hid_n_keyboard_report(PS_KBD_INSTANCE, 0, 0, NULL);
    }
    ps_was_pressed      = false;
    ps_press_time       = 0;
    long_press_fired    = false;
    key_release_pending = false;
    key_release_time    = 0;
    last_high_time      = 0;
    is_ps_pressed       = false;
}

void ps_shortcut_tick(const uint8_t *data, uint16_t len) {
    if (len < 10) return;
    if (!get_config().ps_shortcut_enabled) return;

    uint32_t now = to_ms_since_boot(get_absolute_time());
    bool raw_ps = (data[9] & 0x01) != 0;

    if (raw_ps) {
        is_ps_pressed = true;
        last_high_time = now;
    } else if (now - last_high_time > 50) {
        is_ps_pressed = false;
    }

    if (key_release_pending && now >= key_release_time) {
        if (tud_hid_n_ready(PS_KBD_INSTANCE)) {
            tud_hid_n_keyboard_report(PS_KBD_INSTANCE, 0, 0, NULL);
            key_release_pending = false;
        }
    }

    if (is_ps_pressed && !ps_was_pressed) {
        ps_press_time    = now;
        ps_was_pressed   = true;
        long_press_fired = false;
    } else if (is_ps_pressed && ps_was_pressed) {
        if (!long_press_fired && (now - ps_press_time >= 750)) {
            if (tud_hid_n_ready(PS_KBD_INSTANCE)) {
                uint8_t keys[6] = { HID_KEY_TAB };
                tud_hid_n_keyboard_report(PS_KBD_INSTANCE, 0, KEYBOARD_MODIFIER_LEFTGUI, keys);
                long_press_fired    = true;
                key_release_pending = true;
                key_release_time    = now + 30;
            }
        }
    } else if (!is_ps_pressed && ps_was_pressed) {
        if (!long_press_fired) {
            if (tud_hid_n_ready(PS_KBD_INSTANCE)) {
                uint8_t keys[6] = { HID_KEY_G };
                tud_hid_n_keyboard_report(PS_KBD_INSTANCE, 0, KEYBOARD_MODIFIER_LEFTGUI, keys);
                key_release_pending = true;
                key_release_time    = now + 30;
            }
        }
        ps_was_pressed = false;
    }
}
