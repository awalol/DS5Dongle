#include "button_shortcut.h"

#include <cstring>
#include "usb.h"
#include "wake.h"
#include "bt.h"
#include "button_utils.h"
#include "config.h"
#include "tusb.h"
#include "pico/time.h"
#include "utils.h"

// Kept outside the ENABLE_WAKE_HID guard below: config_load() normalizes the
// stored slots on every boot regardless of the wake HID interface.
bool shortcut_slot_valid(const ButtonShortcut &shortcut) {
    if (shortcut.trigger_a >= Disable) return false;
    if (shortcut.flags & ~SHORTCUT_FLAG_MASK) return false;
    if ((shortcut.flags & SHORTCUT_FLAG_HOLD) &&
        (shortcut.action != ShortcutActionKeyboard ||
         (shortcut.flags & SHORTCUT_FLAG_DOUBLE_TAP) ||
         shortcut.trigger_b == SHORTCUT_TRIGGER_DOUBLE_TAP)) return false;
    if (shortcut.trigger_b == SHORTCUT_TRIGGER_TAP ||
        shortcut.trigger_b == SHORTCUT_TRIGGER_DOUBLE_TAP) {
        // Tap slots spell their tap count out in trigger_b.
        if (shortcut.flags & SHORTCUT_FLAG_DOUBLE_TAP) return false;
    } else {
        if (shortcut.trigger_b >= Disable || shortcut.trigger_a == shortcut.trigger_b) {
            return false;
        }
        // The DPad reports a single direction, so two directions can never be held
        // at once and such a chord would silently never fire.
        if (shortcut.trigger_a <= DPadNorthWest && shortcut.trigger_b <= DPadNorthWest) {
            return false;
        }
    }

    switch (shortcut.action) {
        case ShortcutActionKeyboard:
            return shortcut.keyboard.key <= SHORTCUT_KEY_USAGE_MAX &&
                   (shortcut.keyboard.modifiers != 0 || shortcut.keyboard.key != 0);
        case ShortcutActionConsumer:
            return shortcut.consumer.usage != 0 &&
                   shortcut.consumer.usage <= SHORTCUT_CONSUMER_USAGE_MAX;
        case ShortcutActionBtDisconnect:
            return true;
    }
    return false;
}

#ifdef ENABLE_WAKE_HID

static constexpr uint8_t KEYBOARD_INSTANCE = 1;
static constexpr uint8_t CONSUMER_INSTANCE = 2;
static constexpr uint32_t KEY_PRESS_MS = 30;

static bool release_pending = false;
static absolute_time_t release_time = nil_time;
static uint8_t release_instance = KEYBOARD_INSTANCE; // Which interface owes a release report.
static uint16_t pending_mask = 0; // Triggered slots waiting to send their HID report.

static_assert(BUTTON_SHORTCUT_COUNT <= 16, "slot masks are 16 bits wide");

// A slot is "engaged" while its trigger is pressed, which for a chord means both
// buttons held; taps and chords share one state machine from there on.
static uint8_t tap_count[BUTTON_SHORTCUT_COUNT]{};
static absolute_time_t tap_deadline[BUTTON_SHORTCUT_COUNT]{};
static uint16_t engaged_mask = 0; // Last-tick engagement of each slot; fire on 0 -> 1.
// Tap slots whose current press was taken by a chord on the same button. Sticky
// until the button comes back up, so a chord that has already come and gone
// still keeps the lone-tap action from firing on release.
static uint16_t chord_claimed_mask = 0;

// Merge physical holds and the existing timed pulse into one keyboard report.
struct KeyboardState {
    uint8_t modifiers = 0;
    bool keys[SHORTCUT_KEY_USAGE_MAX + 1]{};
};
static KeyboardState held_keyboard;
static uint8_t pulse_modifiers = 0, pulse_key = 0;
static uint8_t sent_modifiers = 0, sent_keys[6]{};
static bool keyboard_clear_pending = false;
static bool consumer_clear_pending = false;
static bool keyboard_dirty = false;
static bool wake_owned_keyboard = false;

static bool shortcuts_available() {
    return !usb_keyboard_only && !usb_reconfiguring && tud_mounted() && !tud_suspended();
}

static void start_pulse(uint8_t instance) {
    release_pending = true;
    release_instance = instance;
    release_time = make_timeout_time_ms(KEY_PRESS_MS);
}

static bool send_keyboard_state(bool include_pulse) {
    if (!keyboard_dirty && !keyboard_clear_pending) return true;
    if (!tud_hid_n_ready(KEYBOARD_INSTANCE)) return false;
    uint8_t modifiers = keyboard_clear_pending ? 0 : held_keyboard.modifiers;
    uint8_t keys[6]{};
    unsigned count = 0;
    if (!keyboard_clear_pending) {
        if (include_pulse) modifiers |= pulse_modifiers;
        for (unsigned key = 1; key <= SHORTCUT_KEY_USAGE_MAX; ++key) {
            if (held_keyboard.keys[key] || (include_pulse && pulse_key == key)) {
                if (count < 6) keys[count] = key;
                ++count;
            }
        }
        // Standard boot-keyboard rollover: never silently drop a held key.
        if (count > 6) memset(keys, 1, sizeof(keys));
    }
    if (!keyboard_clear_pending && modifiers == sent_modifiers &&
        memcmp(keys, sent_keys, sizeof(keys)) == 0) {
        keyboard_dirty = false;
        return true;
    }
    if (!tud_hid_n_keyboard_report(KEYBOARD_INSTANCE, 0, modifiers, keys)) return false;
    sent_modifiers = modifiers;
    memcpy(sent_keys, keys, sizeof(keys));
    // A reset's all-up report may have overtaken freshly sampled holds. Rebuild
    // once more afterward instead of treating that clear as the desired state.
    keyboard_dirty = keyboard_clear_pending;
    keyboard_clear_pending = false;
    return true;
}

// Triggers are ButtonIds below Disable (27), so one word holds them all.
static constexpr uint32_t button_bit(uint8_t button) {
    return button < Disable ? (1u << button) : 0u;
}

static bool is_tap_slot(const ButtonShortcut &shortcut) {
    return shortcut.trigger_b == SHORTCUT_TRIGGER_TAP ||
           shortcut.trigger_b == SHORTCUT_TRIGGER_DOUBLE_TAP;
}

// Both hold and pulse mappings use the same physical trigger semantics.
static bool trigger_engaged(const USBGetStateData &state, const ButtonShortcut &shortcut) {
    return button_is_pressed(state, shortcut.trigger_a) &&
           (is_tap_slot(shortcut) || button_is_pressed(state, shortcut.trigger_b));
}

// Taps spell the count out in trigger_b, chords in the flags byte.
static bool wants_double(const ButtonShortcut &shortcut) {
    return shortcut.trigger_b == SHORTCUT_TRIGGER_DOUBLE_TAP ||
           (shortcut.flags & SHORTCUT_FLAG_DOUBLE_TAP);
}

// Same physical trigger? Chords are an unordered pair, so Create+Options and
// Options+Create are the same chord.
static bool same_trigger(const ButtonShortcut &a, const ButtonShortcut &b) {
    if (is_tap_slot(a) != is_tap_slot(b)) return false;
    if (is_tap_slot(a)) return a.trigger_a == b.trigger_a;
    return (a.trigger_a == b.trigger_a && a.trigger_b == b.trigger_b) ||
           (a.trigger_a == b.trigger_b && a.trigger_b == b.trigger_a);
}

// A single-press slot may only fire immediately when no other slot wants the
// double press of the same trigger; otherwise it has to wait the window out.
static bool has_double_tap_partner(uint8_t slot, const ButtonShortcut &shortcut) {
    for (uint8_t i = 0; i < BUTTON_SHORTCUT_COUNT; ++i) {
        if (i == slot) continue;
        const auto &other = get_button().shortcuts[i];
        if (!shortcut_slot_valid(other)) continue;
        if (wants_double(other) && same_trigger(shortcut, other)) return true;
    }
    return false;
}

// Release only the timed pulse; keyboard holds remain in the merged report.
static bool send_release(uint8_t instance) {
    if (instance == CONSUMER_INSTANCE) {
        uint16_t usage = 0;
        return tud_hid_n_report(CONSUMER_INSTANCE, 0, &usage, sizeof(usage));
    }
    keyboard_dirty = true;
    return send_keyboard_state(false);
}

static bool send_keyboard(const ButtonShortcut &shortcut) {
    if (release_pending || !tud_hid_n_ready(KEYBOARD_INSTANCE)) return false;

    if (keyboard_clear_pending) return false;
    pulse_modifiers = shortcut.keyboard.modifiers;
    pulse_key = shortcut.keyboard.key;
    keyboard_dirty = true;
    if (!send_keyboard_state(true)) return false;

    start_pulse(KEYBOARD_INSTANCE);
    return true;
}

static bool send_consumer(const ButtonShortcut &shortcut) {
    if (release_pending || consumer_clear_pending || !tud_hid_n_ready(CONSUMER_INSTANCE)) return false;

    // Copy out of the packed struct before taking an address.
    uint16_t usage = shortcut.consumer.usage;
    if (!tud_hid_n_report(CONSUMER_INSTANCE, 0, &usage, sizeof(usage))) return false;

    start_pulse(CONSUMER_INSTANCE);
    return true;
}

// Returns true when the slot's action should fire.
// A "guarded" slot is a tap whose button a configured chord also uses: it cannot
// judge a press while the button is down, because the chord's second button may
// still arrive, so it defers every decision to the release.
static bool tap_tick(uint8_t slot, const ButtonShortcut &shortcut, bool engaged,
                     bool guarded, bool chord_claimed) {
    const uint16_t bit = static_cast<uint16_t>(1u << slot);
    const bool edge = engaged && !(engaged_mask & bit);
    const bool want_double = wants_double(shortcut);

    if (chord_claimed) chord_claimed_mask |= bit;
    if (chord_claimed_mask & bit) {
        // The chord took this press; the tap slot sits out the rest of the hold.
        if (!engaged) {
            chord_claimed_mask &= static_cast<uint16_t>(~bit);
            tap_count[slot] = 0;
        }
        return false;
    }

    if (edge) {
        if (tap_count[slot] == 0) {
            if (!guarded && !want_double && !has_double_tap_partner(slot, shortcut)) {
                return true; // Nothing else claims the double press: fire right away.
            }
            tap_count[slot] = 1;
            tap_deadline[slot] = make_timeout_time_ms(SHORTCUT_TAP_WINDOW_MS);
            return false;
        }
        // Second press inside the window: the double-press slot fires, the
        // single-press slot on the same trigger stands down.
        if (guarded) {
            tap_count[slot] = 2; // Decided on release, in case the chord forms.
            return false;
        }
        tap_count[slot] = 0;
        return want_double;
    }

    if (guarded) {
        // Still held: a chord can still claim this press, so nothing is decided.
        if (engaged || tap_count[slot] == 0) return false;
        if (tap_count[slot] == 2) {
            tap_count[slot] = 0;
            return want_double; // Two presses, and no chord came of either.
        }
        if (!want_double && !has_double_tap_partner(slot, shortcut)) {
            tap_count[slot] = 0;
            return true; // Released without the chord forming, and nothing wants a second press.
        }
        if (time_reached(tap_deadline[slot])) {
            tap_count[slot] = 0;
            return !want_double; // The window closed on a lone press.
        }
        return false;
    }

    if (tap_count[slot] && time_reached(tap_deadline[slot])) {
        tap_count[slot] = 0;
        return !want_double; // The window closed on a lone press.
    }
    return false;
}

static void process_shortcuts(const USBGetStateData &state) {
    KeyboardState next_keyboard;
    uint16_t new_engaged_mask = 0;
    uint16_t valid_mask = 0;
    uint32_t chord_buttons = 0; // Buttons some configured chord uses.
    uint32_t chord_held = 0; // Buttons of the chords held right now.
    bool engaged_now[BUTTON_SHORTCUT_COUNT]{};

    // Chords are resolved first: a tap slot sharing one of their buttons has to
    // know whether a chord took this press before it decides anything.
    for (uint8_t i = 0; i < BUTTON_SHORTCUT_COUNT; ++i) {
        const auto &shortcut = get_button().shortcuts[i];
        if (!shortcut_slot_valid(shortcut)) continue;
        const bool engaged = trigger_engaged(state, shortcut);
        if (shortcut.flags & SHORTCUT_FLAG_HOLD) {
            if (engaged) {
                next_keyboard.modifiers |= shortcut.keyboard.modifiers;
                if (shortcut.keyboard.key) next_keyboard.keys[shortcut.keyboard.key] = true;
            }
            continue; // Holds are independent of tap/chord arbitration.
        }

        const uint16_t bit = static_cast<uint16_t>(1u << i);
        valid_mask |= bit;

        if (!is_tap_slot(shortcut)) {
            const uint32_t buttons =
                    button_bit(shortcut.trigger_a) | button_bit(shortcut.trigger_b);
            chord_buttons |= buttons;
            if (engaged) chord_held |= buttons;
        }
        engaged_now[i] = engaged;
        if (engaged) new_engaged_mask |= bit;
    }

    for (uint8_t i = 0; i < BUTTON_SHORTCUT_COUNT; ++i) {
        const uint16_t bit = static_cast<uint16_t>(1u << i);
        if (!(valid_mask & bit)) continue;

        const auto &shortcut = get_button().shortcuts[i];
        // The chord wins over a lone tap on either of its buttons.
        const bool guarded = is_tap_slot(shortcut) &&
                             (chord_buttons & button_bit(shortcut.trigger_a));
        const bool claimed = guarded && (chord_held & button_bit(shortcut.trigger_a));

        if (tap_tick(i, shortcut, engaged_now[i], guarded, claimed)) pending_mask |= bit;
    }
    if (next_keyboard.modifiers != held_keyboard.modifiers ||
        memcmp(next_keyboard.keys, held_keyboard.keys, sizeof(next_keyboard.keys)) != 0) {
        held_keyboard = next_keyboard;
        keyboard_dirty = true;
    }
    engaged_mask = new_engaged_mask;
    // A slot reconfigured out from under a pending action never gets to fire it.
    pending_mask &= valid_mask;
    chord_claimed_mask &= valid_mask;

}

static void dispatch_pending() {
    if (!pending_mask) return;
    for (uint8_t i = 0; i < BUTTON_SHORTCUT_COUNT; ++i) {
        const uint16_t bit = static_cast<uint16_t>(1u << i);
        if (!(pending_mask & bit)) continue;
        const auto &shortcut = get_button().shortcuts[i];
        bool handled = false;
        switch (shortcut.action) {
            case ShortcutActionKeyboard:
                handled = send_keyboard(shortcut);
                break;
            case ShortcutActionConsumer:
                handled = send_consumer(shortcut);
                break;
            case ShortcutActionBtDisconnect:
                handled = bt_disconnect();
                break;
        }
        if (handled) {
            pending_mask &= static_cast<uint16_t>(~bit);
            // Send at most one action per service pass.
            break;
        }
    }
}

void button_shortcut_reset() {
    held_keyboard = {};
    pulse_modifiers = pulse_key = 0;
    keyboard_clear_pending = true;
    keyboard_dirty = true;
    consumer_clear_pending = consumer_clear_pending ||
        (release_pending && release_instance == CONSUMER_INSTANCE);
    release_pending = false;
    release_instance = KEYBOARD_INSTANCE;
    release_time = nil_time;
    pending_mask = 0;
    engaged_mask = 0;
    chord_claimed_mask = 0;
    for (uint8_t i = 0; i < BUTTON_SHORTCUT_COUNT; ++i) {
        tap_count[i] = 0;
        tap_deadline[i] = nil_time;
    }
}

void button_shortcut_task() {
    // Also called without BT input so endpoint backpressure/disconnect releases retry.
    if (!shortcuts_available()) return;
    if (wake_keyboard_busy()) {
        if (!wake_owned_keyboard) button_shortcut_reset();
        wake_owned_keyboard = true;
        return; // Wake sends reports on this same endpoint; do not overwrite them.
    }
    wake_owned_keyboard = false;
    if (consumer_clear_pending && tud_hid_n_ready(CONSUMER_INSTANCE) &&
        send_release(CONSUMER_INSTANCE)) consumer_clear_pending = false;
    if (keyboard_clear_pending) {
        send_keyboard_state(false);
        return; // Keep the explicit all-up report separate from a new press.
    }
    if (release_pending && time_reached(release_time)) {
        if (tud_hid_n_ready(release_instance) && send_release(release_instance)) {
            release_pending = false;
        }
        // A busy consumer endpoint must not delay a physical keyboard release.
    }
    send_keyboard_state(release_pending && release_instance == KEYBOARD_INSTANCE);
    dispatch_pending(); // Retry queued pulses even if Bluetooth stops reporting.

}

void button_shortcut_tick(const USBGetStateData &state) {
    if (!shortcuts_available() || wake_keyboard_busy()) return;
    process_shortcuts(state);
    button_shortcut_task();
}

#endif // ENABLE_WAKE_HID
