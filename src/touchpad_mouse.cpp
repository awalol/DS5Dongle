// Touchpad-as-mouse: turns DualSense touchpad input into a relative HID mouse
// (Report ID 3 on the consumer-control interface) so the controller can drive
// the host's cursor.
//
//   one finger moves     -> cursor movement (speed + simple acceleration)
//   two fingers move     -> vertical / horizontal scroll (natural direction)
//   touchpad click       -> left button, or right button with two fingers down
//   quick tap            -> left click; two-finger tap -> right click (can be disabled)
//   PS + touchpad click  -> toggle the mouse on/off at runtime
//
// Enabled with touchpad_mouse (1: shared with the gamepad, 2: mouse only) and a
// USB reconnect so the mouse collection is enumerated.

#include "touchpad_mouse.h"

#include <cmath>
#include <cstdlib>

#include "config.h"
#include "tusb.h"
#include "usb.h"
#include "utils.h"
#include "pico/time.h"

bool touchpad_mouse_enumerated = false;

#ifdef ENABLE_WAKE_HID

namespace {

constexpr uint8_t CONSUMER_INSTANCE = 2; // Shared with button_shortcut.cpp

constexpr uint8_t MOUSE_LEFT = 0x01;
constexpr uint8_t MOUSE_RIGHT = 0x02;

// Touch units per wheel notch at scroll speed 10. The touchpad is 1920 x 1080 units.
constexpr float SCROLL_UNITS_PER_NOTCH = 40.0f;
// Speed of a finger (units per report) at which acceleration doubles the gain.
constexpr float ACCEL_REFERENCE = 24.0f;
constexpr float ACCEL_MAX_FACTOR = 3.0f;
// A touch shorter and stiller than this, with no physical click, is a tap.
constexpr uint32_t TAP_MAX_MS = 180;
constexpr int32_t TAP_MAX_TRAVEL = 40;
// How long a tap holds its button down, so every host registers the click.
constexpr uint32_t TAP_HOLD_MS = 20;
// Keeps a backed-up endpoint from accumulating an unbounded jump.
constexpr float ACCUM_LIMIT = 1024.0f;

struct FingerTrack {
    bool valid = false;
    uint8_t index = 0;
    int16_t x = 0;
    int16_t y = 0;
};

enum TapState : uint8_t {
    TapIdle = 0,
    TapPressPending, // Press not sent yet
    TapHeld,         // Press sent; release once tap_release_at is reached
};

bool active = true; // Runtime toggle (PS + touchpad click); starts on.
bool combo_latched = false; // PS + click seen; ignore the click until it is released.

FingerTrack prev[2];

// Current touch session (first finger down until the last finger up).
bool session = false;
bool session_scroll = false; // Two fingers were seen: no cursor motion until all lift.
bool session_clicked = false;
uint8_t session_max_fingers = 0;
uint32_t session_start_ms = 0;
int32_t session_travel = 0;

uint8_t click_buttons = 0; // Physical click, latched to left/right at the press edge.
bool prev_pad = false;

TapState tap_state = TapIdle;
uint8_t tap_buttons = 0;
absolute_time_t tap_release_at = nil_time;

float acc_x = 0, acc_y = 0, acc_wheel = 0, acc_pan = 0;
uint8_t sent_buttons = 0;

bool feature_on() {
    return get_config().touchpad_mouse != TouchpadMouseOff;
}

bool available() {
    return feature_on() && touchpad_mouse_enumerated && !usb_keyboard_only &&
           !usb_reconfiguring && tud_mounted() && !tud_suspended();
}

uint32_t now_ms() {
    return to_ms_since_boot(get_absolute_time());
}

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void reset_motion() {
    for (auto &p : prev) p.valid = false;
    session = session_scroll = session_clicked = false;
    session_max_fingers = 0;
    session_travel = 0;
    click_buttons = 0;
    tap_state = TapIdle;
    tap_buttons = 0;
    acc_x = acc_y = acc_wheel = acc_pan = 0;
}

uint8_t wanted_buttons() {
    uint8_t buttons = click_buttons;
    if (tap_state != TapIdle) buttons |= tap_buttons;
    return buttons;
}

int8_t take(float &acc) {
    const float v = clampf(acc, -127.0f, 127.0f);
    return static_cast<int8_t>(v); // Truncates toward zero; the remainder stays in acc.
}

void send_report() {
    if (!tud_hid_n_ready(CONSUMER_INSTANCE)) return;

    if (tap_state == TapHeld && time_reached(tap_release_at)) tap_state = TapIdle;

    const uint8_t buttons = wanted_buttons();
    const int8_t dx = take(acc_x);
    const int8_t dy = take(acc_y);
    const int8_t wheel = take(acc_wheel);
    const int8_t pan = take(acc_pan);
    if (buttons == sent_buttons && !dx && !dy && !wheel && !pan) return;

    const uint8_t report[5] = {
        buttons,
        static_cast<uint8_t>(dx),
        static_cast<uint8_t>(dy),
        static_cast<uint8_t>(wheel),
        static_cast<uint8_t>(pan),
    };
    if (!tud_hid_n_report(CONSUMER_INSTANCE, TOUCHPAD_MOUSE_REPORT_ID, report, sizeof(report))) return;

    sent_buttons = buttons;
    acc_x -= dx;
    acc_y -= dy;
    acc_wheel -= wheel;
    acc_pan -= pan;
    if (tap_state == TapPressPending) {
        tap_state = TapHeld;
        tap_release_at = make_timeout_time_ms(TAP_HOLD_MS);
    }
}

void handle_toggle(const USBGetStateData &state) {
    const bool pad = state.ButtonPad;
    if (!combo_latched && state.ButtonHome && pad && !prev_pad) {
        combo_latched = true;
        active = !active;
        reset_motion();
    }
    if (combo_latched && !pad) combo_latched = false;
}

void handle_click(bool pad, uint8_t fingers) {
    if (combo_latched) {
        click_buttons = 0;
        return;
    }
    if (pad && !prev_pad) {
        click_buttons = fingers >= 2 ? MOUSE_RIGHT : MOUSE_LEFT;
        session_clicked = true;
    } else if (!pad) {
        click_buttons = 0;
    }
}

void handle_motion(const USBGetStateData &state) {
    bool touching[2];
    int16_t x[2], y[2];
    int16_t dx[2]{}, dy[2]{};
    bool moved[2]{};
    uint8_t fingers = 0;

    for (int i = 0; i < 2; ++i) {
        const auto &f = state.Touch.Finger[i];
        touching[i] = !f.NotTouching;
        x[i] = static_cast<int16_t>(f.FingerX);
        y[i] = static_cast<int16_t>(f.FingerY);
        if (!touching[i]) {
            prev[i].valid = false;
            continue;
        }
        ++fingers;
        if (prev[i].valid && prev[i].index == f.Index) {
            dx[i] = static_cast<int16_t>(x[i] - prev[i].x);
            dy[i] = static_cast<int16_t>(y[i] - prev[i].y);
            moved[i] = true;
        }
        prev[i] = {true, static_cast<uint8_t>(f.Index), x[i], y[i]};
    }

    const uint32_t now = now_ms();
    if (fingers && !session) {
        session = true;
        session_scroll = session_clicked = false;
        session_max_fingers = 0;
        session_travel = 0;
        session_start_ms = now;
    }
    if (fingers > session_max_fingers) session_max_fingers = fingers;

    handle_click(state.ButtonPad, fingers);

    if (fingers >= 2) {
        session_scroll = true;
        if (moved[0] && moved[1]) {
            const float avg_dx = (dx[0] + dx[1]) * 0.5f;
            const float avg_dy = (dy[0] + dy[1]) * 0.5f;
            // Natural scrolling (default): content follows the fingers.
            const auto &config = get_config();
            float notches = config.touchpad_mouse_scroll_speed / (10.0f * SCROLL_UNITS_PER_NOTCH);
            if (config.touchpad_mouse_scroll_invert) notches = -notches;
            acc_wheel = clampf(acc_wheel + avg_dy * notches, -ACCUM_LIMIT, ACCUM_LIMIT);
            acc_pan = clampf(acc_pan - avg_dx * notches, -ACCUM_LIMIT, ACCUM_LIMIT);
            session_travel += std::abs(static_cast<int32_t>(avg_dx)) + std::abs(static_cast<int32_t>(avg_dy));
        }
    } else if (fingers == 1 && !session_scroll) {
        const int i = touching[0] ? 0 : 1;
        if (moved[i]) {
            const float fx = dx[i], fy = dy[i];
            const float speed = std::sqrt(fx * fx + fy * fy);
            const float accel = clampf(1.0f + speed / ACCEL_REFERENCE, 1.0f, ACCEL_MAX_FACTOR);
            const float gain = get_config().touchpad_mouse_speed / 10.0f * accel;
            acc_x = clampf(acc_x + fx * gain, -ACCUM_LIMIT, ACCUM_LIMIT);
            acc_y = clampf(acc_y + fy * gain, -ACCUM_LIMIT, ACCUM_LIMIT);
            session_travel += std::abs(dx[i]) + std::abs(dy[i]);
        }
    }

    if (!fingers && session) {
        // Last finger lifted: a short, still touch without a click is a tap.
        const bool tap = !get_config().touchpad_mouse_tap_disable &&
                         !session_clicked && !combo_latched &&
                         now - session_start_ms <= TAP_MAX_MS &&
                         session_travel <= TAP_MAX_TRAVEL;
        if (tap && tap_state == TapIdle) {
            tap_buttons = session_max_fingers >= 2 ? MOUSE_RIGHT : MOUSE_LEFT;
            tap_state = TapPressPending;
        }
        session = false;
        session_scroll = false;
        // Drop sub-pixel / sub-notch leftovers so the next touch starts clean.
        acc_x = acc_y = acc_wheel = acc_pan = 0;
    }
}

} // namespace

void touchpad_mouse_tick(const USBGetStateData &state) {
    if (!available()) {
        prev_pad = state.ButtonPad;
        return;
    }
    handle_toggle(state);
    if (active) {
        handle_motion(state);
    } else {
        reset_motion();
    }
    prev_pad = state.ButtonPad;
    send_report();
}

void touchpad_mouse_task() {
    if (!available()) {
        // Nothing is sent while unavailable; start from all-up when it comes back.
        reset_motion();
        sent_buttons = 0;
        combo_latched = false;
        return;
    }
    send_report(); // Retry a backed-up endpoint and finish tap releases.
}

void touchpad_mouse_filter_report(USBGetStateData &report) {
    if (!available()) return;
    // The toggle chord is the firmware's, not the game's.
    if (combo_latched) report.ButtonPad = 0;
    if (active && get_config().touchpad_mouse == TouchpadMouseExclusive) {
        report.Touch.Finger[0].NotTouching = 1;
        report.Touch.Finger[1].NotTouching = 1;
        report.ButtonPad = 0;
    }
}

#endif // ENABLE_WAKE_HID
