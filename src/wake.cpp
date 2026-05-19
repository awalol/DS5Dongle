//
// Created by awalol on 2026/4/30.
//

#include "wake.h"

#ifdef ENABLE_WAKE_HID

#include <cstdio>
#include <cstring>
#include "tusb.h"
#include "device/dcd.h"
#include "pico/sync.h"
#include "pico/time.h"

#define WAKE_KBD_INSTANCE     1
#define WAKE_KEYCODE_F15      0x68
// Post-resume timings tuned for "wake-and-resleep" Windows behavior: the host
// resumes USB, but if no HID input is consumed during the brief wake window
// the system can re-suspend within ~1 s. Bigger settles + a second F15 give
// Windows multiple polling cycles to pick the keystroke up.
#define WAKE_SETTLE_US        150000   // 150 ms — let host finish USB re-init
#define WAKE_KEY_HOLD_US       80000   // 80 ms keydown -> keyup gap
#define WAKE_KEY_UP_SETTLE_US 200000   // 200 ms between attempts (or before DONE)
#define WAKE_REQUEST_TIMEOUT_US 5000000
#define WAKE_KEY_ATTEMPTS     2

#ifdef WAKE_DEBUG
#  define WAKE_DBG(fmt, ...) printf("[wake] " fmt "\n", ##__VA_ARGS__)
static const char *wake_state_name(int s) {
    switch (s) {
    case 0: return "IDLE";
    case 1: return "PENDING_PRESS";
    case 2: return "REQUESTED";
    case 3: return "KEY_DOWN";
    case 4: return "KEY_UP_SENT";
    case 5: return "DONE";
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
    WAKE_KEY_DOWN,
    WAKE_KEY_UP_SENT,
    WAKE_DONE,
} wake_state_t;

static critical_section_t wake_cs;
static volatile bool host_suspended = false;
static volatile bool host_resumed_event = false;
static wake_state_t state = WAKE_IDLE;
static uint64_t state_entered_us = 0;
static uint8_t key_attempts = 0;
// Last-seen DualSense button bytes. Idle defaults: byte 7 = 0x08 (D-pad
// released), bytes 8 / 9 = 0 (no shoulders, no PS / touchpad / mute).
static uint8_t prev_b7 = 0x08;
static uint8_t prev_b8 = 0x00;
static uint8_t prev_b9 = 0x00;

static void enter_state(wake_state_t s) {
    state = s;
    state_entered_us = time_us_64();
}

void wake_init(void) {
    critical_section_init(&wake_cs);
}

extern "C" void tud_suspend_cb(bool remote_wakeup_en) {
    WAKE_DBG("tud_suspend_cb remote_wakeup_en=%d prev_state=%s",
             (int)remote_wakeup_en, wake_state_name(state));
    host_suspended = true;
    host_resumed_event = false;
    
    // Unconditionally re-arm on suspend. If a previous wake attempt hung
    // (e.g. Linux ignored a keystroke and left the endpoint busy forever),
    // we must abort and reset so the NEXT wake attempt can trigger.
    state = WAKE_PENDING_PRESS;
    state_entered_us = time_us_64();
    prev_b7 = 0x08; prev_b8 = 0x00; prev_b9 = 0x00;
    key_attempts = 0;
    WAKE_DBG("-> PENDING_PRESS");
}

extern "C" void tud_resume_cb(void) {
    WAKE_DBG("tud_resume_cb state=%s", wake_state_name(state));
    host_suspended = false;
    host_resumed_event = true;
}

extern "C" void tud_mount_cb(void) {
    WAKE_DBG("tud_mount_cb state=%s", wake_state_name(state));
    host_suspended = false;
    host_resumed_event = true;
}

void wake_on_bt_input(const uint8_t *hid_input, uint16_t len) {
    if (len < 10) return;
    // DualSense BT 0x31 input report layout (after main.cpp's `data + 3` skip):
    //   byte 7 low nibble: D-pad direction (0x08 idle); high nibble: face buttons
    //   byte 8: L1, R1, L2 click, R2 click, share, options, L3, R3
    //   byte 9: PS (bit 0), touchpad-click (bit 1), mute (bit 2)
    //
    // We trigger on ANY change in those three button bytes, not strictly on
    // the PS bit. Reasons:
    //   1. The DualSense's BT radio enters a low-power sniff mode after a
    //      period of inactivity. The PS button alone often does not wake
    //      the radio out of sniff -- shoulder buttons reliably do. So the
    //      first BT report after S3 is most likely whichever button the
    //      user happened to press to wake the radio. PS itself counts as
    //      "any button" too, so the single-press UX still works.
    //   2. We additionally call tud_remote_wakeup() speculatively even from
    //      WAKE_IDLE / WAKE_DONE state. TinyUSB returns true only when the
    //      host actually USB-suspended the bus; otherwise it's a no-op. This
    //      protects against the case where tud_suspend_cb didn't fire (e.g.
    //      a hub between the host and the dongle masking the suspend signal
    //      from downstream). On success the FSM transitions to REQUESTED and
    //      proceeds with the keystroke as normal.
    const uint8_t b7 = hid_input[7];
    const uint8_t b8 = hid_input[8];
    const uint8_t b9 = hid_input[9];

    critical_section_enter_blocking(&wake_cs);
    const bool changed = (b7 != prev_b7) || (b8 != prev_b8) || (b9 != prev_b9);
    const bool armable = (state == WAKE_IDLE || state == WAKE_DONE || state == WAKE_PENDING_PRESS);
    prev_b7 = b7; prev_b8 = b8; prev_b9 = b9;
    critical_section_exit(&wake_cs);

    if (changed && armable) {
        bool ok = tud_remote_wakeup();
        
        // Linux quirk: Sometimes Linux fails to set the REMOTE_WAKEUP feature
        // flag before the second suspend, causing TinyUSB to refuse to wake.
        // If we are suspended but ok is false, we force the wake signal.
        if (!ok && host_suspended) {
            WAKE_DBG("tud_remote_wakeup()=0 but suspended. Forcing DCD wake.");
            dcd_remote_wakeup(0);
            ok = true;
        }

        if (ok) {
            critical_section_enter_blocking(&wake_cs);
            state = WAKE_REQUESTED;
            state_entered_us = time_us_64();
            critical_section_exit(&wake_cs);
            WAKE_DBG("button event -> REQUESTED, tud_remote_wakeup()=1");
        }
#ifdef WAKE_DEBUG
        else {
            static uint64_t last_log = 0;
            const uint64_t now = time_us_64();
            if (now - last_log > 5000000) {
                WAKE_DBG("button event, tud_remote_wakeup()=0 (USB bus not in suspend) -- 5s heartbeat");
                last_log = now;
            }
        }
#endif
    }
}

void wake_on_bt_disconnect(void) {
    critical_section_enter_blocking(&wake_cs);
    state = WAKE_IDLE;
    prev_b7 = 0x08; prev_b8 = 0x00; prev_b9 = 0x00;
    critical_section_exit(&wake_cs);
}

void wake_task(void) {
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
            if (host_resumed_event || !host_suspended) {
                host_resumed_event = false;
                if (now - entered < WAKE_SETTLE_US) return;
                if (!tud_hid_n_ready(WAKE_KBD_INSTANCE)) {
#ifdef WAKE_DEBUG
                    static uint64_t last_log = 0;
                    if (now - last_log > 1000000) {
                        WAKE_DBG("REQUESTED waiting: hid_n_ready=0 (heartbeat 1Hz)");
                        last_log = now;
                    }
#endif
                    return;
                }
                uint8_t rpt[8] = { 0, 0, WAKE_KEYCODE_F15, 0, 0, 0, 0, 0 };
                const bool sent = tud_hid_n_report(WAKE_KBD_INSTANCE, 0, rpt, sizeof(rpt));
                WAKE_DBG("REQUESTED: sent keydown 0x%02X -> %d", WAKE_KEYCODE_F15, (int)sent);
                if (sent) {
                    critical_section_enter_blocking(&wake_cs);
                    enter_state(WAKE_KEY_DOWN);
                    critical_section_exit(&wake_cs);
                }
            } else if (now - entered > WAKE_REQUEST_TIMEOUT_US) {
                WAKE_DBG("REQUESTED timeout 5s -> DONE (no resume signaling; may have already woken)");
                critical_section_enter_blocking(&wake_cs);
                enter_state(WAKE_DONE);
                critical_section_exit(&wake_cs);
            }
            return;
        }

        case WAKE_KEY_DOWN: {
            if (now - entered < WAKE_KEY_HOLD_US) return;
            if (!tud_hid_n_ready(WAKE_KBD_INSTANCE)) {
#ifdef WAKE_DEBUG
                static uint64_t last_log = 0;
                if (now - last_log > 1000000) {
                    WAKE_DBG("KEY_DOWN waiting: hid_n_ready=0 (heartbeat 1Hz)");
                    last_log = now;
                }
#endif
                return;
            }
            uint8_t up[8] = { 0 };
            const bool sent = tud_hid_n_report(WAKE_KBD_INSTANCE, 0, up, sizeof(up));
            WAKE_DBG("KEY_DOWN: sent keyup -> %d", (int)sent);
            if (sent) {
                critical_section_enter_blocking(&wake_cs);
                enter_state(WAKE_KEY_UP_SENT);
                critical_section_exit(&wake_cs);
            }
            return;
        }

        case WAKE_KEY_UP_SENT: {
            if (now - entered < WAKE_KEY_UP_SETTLE_US) return;
            key_attempts++;
            if (key_attempts < WAKE_KEY_ATTEMPTS) {
                // Retry: do NOT re-enter WAKE_REQUESTED (which gates on a
                // fresh tud_resume_cb event). We already established the
                // host woke once; just send another keydown directly. If the
                // host has dipped back into suspend, tud_hid_n_ready will be
                // false and we'll heartbeat from KEY_DOWN until it returns.
                if (!tud_hid_n_ready(WAKE_KBD_INSTANCE)) {
#ifdef WAKE_DEBUG
                    static uint64_t last_log = 0;
                    if (now - last_log > 1000000) {
                        WAKE_DBG("KEY_UP_SENT retry waiting: hid_n_ready=0 (heartbeat 1Hz)");
                        last_log = now;
                    }
#endif
                    return;
                }
                uint8_t rpt[8] = { 0, 0, WAKE_KEYCODE_F15, 0, 0, 0, 0, 0 };
                const bool sent = tud_hid_n_report(WAKE_KBD_INSTANCE, 0, rpt, sizeof(rpt));
                WAKE_DBG("KEY_UP_SENT: retrying F15 (attempt %d/%d) -> %d",
                         (int)key_attempts + 1, (int)WAKE_KEY_ATTEMPTS, (int)sent);
                if (sent) {
                    critical_section_enter_blocking(&wake_cs);
                    enter_state(WAKE_KEY_DOWN);
                    critical_section_exit(&wake_cs);
                }
            } else {
                WAKE_DBG("KEY_UP_SENT settle done -> DONE");
                critical_section_enter_blocking(&wake_cs);
                enter_state(WAKE_DONE);
                key_attempts = 0;
                critical_section_exit(&wake_cs);
            }
            return;
        }
    }
}

#endif // ENABLE_WAKE_HID
