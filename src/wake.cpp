//
// Created by awalol on 2026/4/30.
//
// Wake-on-PS: controller button -> USB remote wakeup -> forward DS5 gamepad
// input to the host. No keyboard interface or synthetic key injection.

#include "wake.h"

#ifdef ENABLE_WAKE_HID

#include <cstdio>
#include <cstring>
#include "bt.h"
#include "tusb.h"
#include "device/dcd.h"
#include "pico/sync.h"
#include "pico/time.h"
#include "ps_shortcut.h"
#include "config.h"
#include "state_mgr.h"
#include "audio.h"

extern bool spk_active;

#define WAKE_GAMEPAD_REPORT_LEN 63
// Post-resume settle: let the host finish USB re-init before the gamepad report.
#define WAKE_SETTLE_US          150000
#define WAKE_REPORT_RETRY_US    200000
#define WAKE_REQUEST_TIMEOUT_US 5000000
#define WAKE_REPORT_ATTEMPTS    2

#ifdef WAKE_DEBUG
#  define WAKE_DBG(fmt, ...) printf("[wake] " fmt "\n", ##__VA_ARGS__)
static const char *wake_state_name(int s) {
    switch (s) {
    case 0: return "IDLE";
    case 1: return "PENDING_PRESS";
    case 2: return "REQUESTED";
    case 3: return "DONE";
    default: return "?";
    }
}
#else
#  define WAKE_DBG(fmt, ...) ((void)0)
#endif

typedef enum {
    WAKE_IDLE,
    WAKE_PENDING_PRESS,
    WAKE_REQUESTED,
    WAKE_DONE,
} wake_state_t;

static critical_section_t wake_cs;
static volatile bool host_suspended = false;
static volatile bool host_resumed_event = false;
static wake_state_t state = WAKE_IDLE;
static uint64_t state_entered_us = 0;
static uint8_t report_attempts = 0;
static uint8_t pending_gamepad[WAKE_GAMEPAD_REPORT_LEN]{};
static bool pending_gamepad_valid = false;
// Last-seen DualSense button bytes. Idle defaults: byte 7 = 0x08 (D-pad
// released), bytes 8 / 9 = 0 (no shoulders, no PS / touchpad / mute).
static uint8_t prev_b7 = 0x08;
static uint8_t prev_b8 = 0x00;
static uint8_t prev_b9 = 0x00;

static void enter_state(wake_state_t s) {
    state = s;
    state_entered_us = time_us_64();
}

static void stash_gamepad_report(const uint8_t *hid_input, uint16_t len) {
    const uint16_t copy_len = len < WAKE_GAMEPAD_REPORT_LEN ? len : WAKE_GAMEPAD_REPORT_LEN;
    memcpy(pending_gamepad, hid_input, copy_len);
    if (copy_len < WAKE_GAMEPAD_REPORT_LEN) {
        memset(pending_gamepad + copy_len, 0, WAKE_GAMEPAD_REPORT_LEN - copy_len);
    }
    pending_gamepad_valid = true;
}

static void request_host_wake(const char *reason, const uint8_t *hid_input, uint16_t len) {
    if (hid_input != nullptr && len > 0) {
        stash_gamepad_report(hid_input, len);
    }

    bool ok = tud_remote_wakeup();

    // Linux quirk: host may not set REMOTE_WAKEUP before a repeat suspend.
    if (!ok && host_suspended) {
        WAKE_DBG("%s: tud_remote_wakeup()=0 but suspended. Forcing DCD wake.", reason);
        dcd_remote_wakeup(0);
        ok = true;
    }

    if (ok) {
        critical_section_enter_blocking(&wake_cs);
        state = WAKE_REQUESTED;
        state_entered_us = time_us_64();
        report_attempts = 0;
        critical_section_exit(&wake_cs);
        WAKE_DBG("%s -> REQUESTED", reason);
    }
#ifdef WAKE_DEBUG
    else {
        static uint64_t last_log = 0;
        const uint64_t now = time_us_64();
        if (now - last_log > 5000000) {
            WAKE_DBG("%s, tud_remote_wakeup()=0 (USB bus not in suspend) -- 5s heartbeat", reason);
            last_log = now;
        }
    }
#endif
}

void wake_init(void) {
    critical_section_init(&wake_cs);
}

extern "C" void tud_suspend_cb(bool remote_wakeup_en) {
    WAKE_DBG("tud_suspend_cb remote_wakeup_en=%d prev_state=%s",
             (int)remote_wakeup_en, wake_state_name(state));
    // Required: tell the controller to drop BT during host USB suspend. Wake
    // is handled on the next controller button press -> BT reconnect ->
    // wake_on_bt_connect() / wake_on_bt_input().
    bt_power_off_controller();
    host_suspended = true;
    host_resumed_event = false;
    if (get_config().enable_wake) {
        state_note_usb_suspend();
    }

    state = WAKE_PENDING_PRESS;
    state_entered_us = time_us_64();
    prev_b7 = 0x08;
    prev_b8 = 0x00;
    prev_b9 = 0x00;
    report_attempts = 0;
    WAKE_DBG("-> PENDING_PRESS");
}

void wake_on_bt_connect(void) {
    if (!get_config().enable_wake) return;
    critical_section_enter_blocking(&wake_cs);
    const bool should_wake = host_suspended &&
        (state == WAKE_IDLE || state == WAKE_DONE || state == WAKE_PENDING_PRESS);
    critical_section_exit(&wake_cs);

    if (should_wake) {
        request_host_wake("BT reconnect while suspended", nullptr, 0);
    }
    if (bt_is_connected() && spk_active) {
        audio_resync_speaker_path();
    }
}

extern "C" void tud_resume_cb(void) {
    WAKE_DBG("tud_resume_cb state=%s", wake_state_name(state));
    host_suspended = false;
    host_resumed_event = true;
    if (get_config().enable_wake) {
        state_on_host_usb_resume();
    }
    // Re-sync controller audio after USB resume (BT may reconnect shortly after).
    if (bt_is_connected()) {
        if (spk_active) {
            audio_resync_speaker_path();
        } else {
            update_mic_status();
        }
    }
}

extern "C" void tud_mount_cb(void) {
    WAKE_DBG("tud_mount_cb state=%s", wake_state_name(state));
    host_suspended = false;
    host_resumed_event = true;
    config_note_usb_enumerated_layout();
}

void wake_on_bt_input(const uint8_t *hid_input, uint16_t len) {
    if (!get_config().enable_wake) return;
    if (len < 10) return;

    const uint8_t b7 = hid_input[7];
    const uint8_t b8 = hid_input[8];
    const uint8_t b9 = hid_input[9];

    critical_section_enter_blocking(&wake_cs);
    const bool changed = (b7 != prev_b7) || (b8 != prev_b8) || (b9 != prev_b9);
    const bool armable = (state == WAKE_IDLE || state == WAKE_DONE || state == WAKE_PENDING_PRESS);
    prev_b7 = b7;
    prev_b8 = b8;
    prev_b9 = b9;
    critical_section_exit(&wake_cs);

    if (changed && armable) {
        request_host_wake("button event", hid_input, len);
    }
}

void wake_on_bt_disconnect(void) {
    critical_section_enter_blocking(&wake_cs);
    state = WAKE_IDLE;
    prev_b7 = 0x08;
    prev_b8 = 0x00;
    prev_b9 = 0x00;
    pending_gamepad_valid = false;
    critical_section_exit(&wake_cs);
    ps_shortcut_reset();
}

static bool push_gamepad_wake_report(void) {
    if (!pending_gamepad_valid) {
        return true;
    }
    if (!tud_hid_ready()) {
        return false;
    }
    if (!tud_hid_report(0x01, pending_gamepad, WAKE_GAMEPAD_REPORT_LEN)) {
        return false;
    }
    WAKE_DBG("sent gamepad wake report (attempt %d/%d)", report_attempts + 1, WAKE_REPORT_ATTEMPTS);
    return true;
}

void wake_task(void) {
    if (!get_config().enable_wake) return;
    const uint64_t now = time_us_64();

    critical_section_enter_blocking(&wake_cs);
    const wake_state_t s = state;
    const uint64_t entered = state_entered_us;
    critical_section_exit(&wake_cs);

    switch (s) {
        case WAKE_IDLE:
        case WAKE_PENDING_PRESS:
        case WAKE_DONE:
            return;

        case WAKE_REQUESTED: {
            if (!(host_resumed_event || !host_suspended)) {
                if (now - entered > WAKE_REQUEST_TIMEOUT_US) {
                    WAKE_DBG("REQUESTED timeout 5s -> DONE");
                    critical_section_enter_blocking(&wake_cs);
                    enter_state(WAKE_DONE);
                    critical_section_exit(&wake_cs);
                }
                return;
            }

            host_resumed_event = false;
            if (now - entered < WAKE_SETTLE_US) {
                return;
            }

            if (report_attempts > 0 && now - entered < WAKE_REPORT_RETRY_US) {
                return;
            }

            if (!push_gamepad_wake_report()) {
#ifdef WAKE_DEBUG
                static uint64_t last_log = 0;
                if (now - last_log > 1000000) {
                    WAKE_DBG("REQUESTED waiting: tud_hid_ready/report (heartbeat 1Hz)");
                    last_log = now;
                }
#endif
                return;
            }

            report_attempts++;
            if (report_attempts < WAKE_REPORT_ATTEMPTS) {
                critical_section_enter_blocking(&wake_cs);
                state_entered_us = time_us_64();
                critical_section_exit(&wake_cs);
                return;
            }

            critical_section_enter_blocking(&wake_cs);
            pending_gamepad_valid = false;
            enter_state(WAKE_DONE);
            critical_section_exit(&wake_cs);
            WAKE_DBG("gamepad wake complete -> DONE");
            return;
        }
    }
}

#endif // ENABLE_WAKE_HID
