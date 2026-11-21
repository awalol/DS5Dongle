// Analog stick directions as shortcut triggers (compiled by tests/test_hold.py).
#include "platform.h"
#include <cassert>

static constexpr uint8_t HOME = 20;

static USBGetStateData stick(uint32_t buttons, uint8_t lx = 128, uint8_t ly = 128,
                             uint8_t rx = 128, uint8_t ry = 128) {
    USBGetStateData s{buttons};
    s.LeftStickX = lx; s.LeftStickY = ly; s.RightStickX = rx; s.RightStickY = ry;
    return s;
}

static ButtonShortcut consumer(uint8_t a, uint8_t b, uint16_t usage) {
    ButtonShortcut s{};
    s.trigger_a = a;
    s.trigger_b = b;
    s.action = ShortcutActionConsumer;
    s.consumer.usage = usage;
    return s;
}

int main() {
    for (auto& s : config.shortcuts) s.trigger_a = BUTTON_SHORTCUT_DISABLED;

    // Validation: every direction is a valid trigger; opposite directions of one
    // stick cannot form a chord; ids between Disable and the sticks stay invalid.
    for (uint8_t t = SHORTCUT_STICK_FIRST; t <= SHORTCUT_STICK_LAST; ++t) {
        assert(shortcut_trigger_valid(t));
        assert(shortcut_slot_valid(consumer(HOME, t, 0xe9)));
        assert(shortcut_slot_valid(consumer(t, SHORTCUT_TRIGGER_TAP, 0xe9)));
    }
    assert(!shortcut_trigger_valid(27) && !shortcut_trigger_valid(SHORTCUT_STICK_LAST + 1));
    assert(!shortcut_slot_valid(consumer(HOME, 27, 0xe9)));
    assert(!shortcut_slot_valid(consumer(ShortcutLeftStickUp, ShortcutLeftStickDown, 0xe9)));
    assert(!shortcut_slot_valid(consumer(ShortcutRightStickRight, ShortcutRightStickLeft, 0xe9)));
    assert(shortcut_slot_valid(consumer(ShortcutLeftStickUp, ShortcutLeftStickLeft, 0xe9)));
    assert(shortcut_slot_valid(consumer(ShortcutLeftStickUp, ShortcutRightStickDown, 0xe9)));

    // Home + Left stick up -> Volume Up; Home + Left stick down -> Volume Down.
    config.shortcuts[0] = consumer(HOME, ShortcutLeftStickUp, 0xe9);
    config.shortcuts[1] = consumer(HOME, ShortcutLeftStickDown, 0xea);
    auto tick = [](const USBGetStateData& s) { button_shortcut_tick(s); };
    auto release = [] { now += 31; button_shortcut_task(); };

    tick(stick(1u << HOME)); assert(consumer_usage == 0);
    tick(stick(1u << HOME, 128, 128 - 70)); assert(consumer_usage == 0); // below press
    tick(stick(1u << HOME, 128, 128 - 80)); assert(consumer_usage == 0xe9);
    release(); assert(consumer_usage == 0);
    // Hysteresis: easing back to 60 keeps it held, so no second pulse.
    tick(stick(1u << HOME, 128, 128 - 60)); tick(stick(1u << HOME, 128, 128 - 85));
    assert(consumer_usage == 0);
    // Dropping under the release threshold lets the next push fire again.
    tick(stick(1u << HOME, 128, 128 - 50)); tick(stick(1u << HOME, 128, 0));
    assert(consumer_usage == 0xe9);
    release();
    // Down is the high end of the Y axis.
    tick(stick(1u << HOME)); tick(stick(1u << HOME, 128, 255)); assert(consumer_usage == 0xea);
    release();
    // The stick alone (no Home) does nothing; the right stick is not the left.
    tick(stick(0)); tick(stick(0, 128, 0)); assert(consumer_usage == 0);
    tick(stick(0)); tick(stick(1u << HOME, 128, 128, 128, 0)); assert(consumer_usage == 0);

    // A Home tap slot is claimed by the Home + stick chord, not fired on release.
    tick(stick(0));
    config.shortcuts[2] = consumer(HOME, SHORTCUT_TRIGGER_TAP, 0xe2);
    tick(stick(1u << HOME)); tick(stick(1u << HOME, 128, 0)); assert(consumer_usage == 0xe9);
    release(); tick(stick(1u << HOME)); tick(stick(0)); now += 300; tick(stick(0));
    assert(consumer_usage == 0);
    // ...while a plain Home tap still mutes.
    tick(stick(1u << HOME)); tick(stick(0)); assert(consumer_usage == 0xe2);
    release();

    // Stick direction as a lone tap trigger.
    for (auto& s : config.shortcuts) s.trigger_a = BUTTON_SHORTCUT_DISABLED;
    config.shortcuts[0] = consumer(ShortcutRightStickLeft, SHORTCUT_TRIGGER_TAP, 0xb6);
    tick(stick(0)); tick(stick(0, 128, 128, 0)); assert(consumer_usage == 0xb6);
    release(); assert(consumer_usage == 0);
}
