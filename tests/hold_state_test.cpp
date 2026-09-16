#include "platform.h"
#include <cassert>

static void slot(unsigned i, uint8_t button, uint8_t modifiers, uint8_t key,
                 uint8_t flags = SHORTCUT_FLAG_HOLD) {
    auto& s = config.shortcuts[i];
    s = {};
    s.trigger_a = button;
    s.trigger_b = SHORTCUT_TRIGGER_TAP;
    s.keyboard.modifiers = modifiers;
    s.keyboard.key = key;
    s.flags = flags;
    assert(shortcut_slot_valid(s));
}
static void tick(uint32_t buttons) { button_shortcut_tick({buttons}); }
int main() {
    for (auto& s : config.shortcuts) s.trigger_a = BUTTON_SHORTCUT_DISABLED;
    slot(0, 8, 1, 6); // Ctrl+C
    slot(1, 9, 1, 25); // Ctrl+V
    tick(1u << 8); assert(mods == 1 && keys[0] == 6);
    unsigned n = reports;
    tick(1u << 8); assert(reports == n); // no duplicate reports
    tick((1u << 8) | (1u << 9)); assert(keys[0] == 6 && keys[1] == 25);
    tick(1u << 9); assert(mods == 1 && keys[0] == 25 && keys[1] == 0);
    ready = false; tick(0); assert(mods == 1);
    ready = true; button_shortcut_task(); assert(mods == 0 && keys[0] == 0);
    slot(1, 9, 1, 6); // same key shared by both sources
    tick((1u << 8) | (1u << 9)); tick(1u << 9);
    assert(mods == 1 && keys[0] == 6 && keys[1] == 0);
    // Timed pulse merges with a hold, and releases only its own keys/modifiers.
    slot(2, 10, 2, 4, 0);
    tick((1u << 9) | (1u << 10)); assert(mods == 3 && keys[0] == 4 && keys[1] == 6);
    now += 31; button_shortcut_task(); assert(mods == 1 && keys[0] == 6);
    // Reset release retries even without further Bluetooth input.
    ready = false; button_shortcut_reset(); button_shortcut_task(); assert(mods == 1);
    ready = true; accept = false; button_shortcut_task(); assert(mods == 1);
    accept = true; button_shortcut_task(); assert(mods == 0 && keys[0] == 0);
    config.shortcuts[0].trigger_b = 11; // two-button hold
    tick(1u << 8); assert(mods == 0);
    tick((1u << 8) | (1u << 11)); assert(mods == 1);
    tick(1u << 11); assert(mods == 0);
    for (unsigned i = 0; i < 7; ++i) slot(i, 8 + i, 0, 4 + i);
    tick(0x7fu << 8); for (auto k : keys) assert(k == 1);
    tick(1u << 8); assert(keys[0] == 4 && keys[1] == 0);
    tick(0); assert(keys[0] == 0);
    suspended = true; tick(1u << 8); assert(keys[0] == 0);
    suspended = false;
    // Failed press submission is serviced without another input packet.
    button_shortcut_reset(); button_shortcut_task();
    for (auto& s : config.shortcuts) s.trigger_a = BUTTON_SHORTCUT_DISABLED;
    slot(0, 8, 0, 4, 0);
    accept = false; tick(1u << 8); assert(keys[0] == 0);
    accept = true; button_shortcut_task(); assert(keys[0] == 4);
    now += 31; button_shortcut_task(); assert(keys[0] == 0);
    // Consumer release retries must not stall keyboard holds/releases.
    slot(1, 9, 1, 6);
    slot(2, 10, 0, 4, 0);
    config.shortcuts[2].action = ShortcutActionConsumer;
    config.shortcuts[2].consumer.usage = 0xe9;
    tick((1u << 9) | (1u << 10)); assert(mods == 1 && consumer_usage == 0xe9);
    consumer_ready = false; now += 31;
    tick(0); assert(mods == 0 && keys[0] == 0 && consumer_usage == 0xe9);
    consumer_ready = true; button_shortcut_task(); assert(consumer_usage == 0);
    // Wake owns the same endpoint; shortcut reports wait until it finishes.
    tick(1u << 9); assert(mods == 1);
    wake_busy = true; button_shortcut_task();
    mods = 0; keys[0] = 0x68; // wake's F15 report
    n = reports; tick(1u << 9); button_shortcut_task();
    assert(reports == n && keys[0] == 0x68);
    wake_busy = false; button_shortcut_task(); assert(keys[0] == 0);
    tick(1u << 9); assert(mods == 1 && keys[0] == 6);
    // Input arriving before a deferred reset clear still needs a later down report.
    button_shortcut_reset();
    tick(1u << 9); assert(mods == 0 && keys[0] == 0);
    button_shortcut_task(); assert(mods == 1 && keys[0] == 6);
    // Realistic endpoint backpressure: two queued pulses must be separated by up.
    button_shortcut_reset(); button_shortcut_task();
    for (auto& s : config.shortcuts) s.trigger_a = BUTTON_SHORTCUT_DISABLED;
    slot(0, 8, 0, 4, 0); slot(1, 9, 0, 5, 0);
    auto_busy = true;
    tick((1u << 8) | (1u << 9)); assert(keys[0] == 4 && !ready);
    now += 31; button_shortcut_task(); assert(keys[0] == 4);
    ready = true; button_shortcut_task(); assert(keys[0] == 0 && !ready);
    ready = true; button_shortcut_task(); assert(keys[0] == 5 && !ready);
    now += 31; ready = true; button_shortcut_task(); assert(keys[0] == 0);
    auto_busy = false; ready = true;
    // Legacy single/double arbitration still selects the double action.
    button_shortcut_reset(); button_shortcut_task();
    slot(0, 8, 0, 4, 0); slot(1, 8, 0, 5, 0);
    config.shortcuts[1].trigger_b = SHORTCUT_TRIGGER_DOUBLE_TAP;
    tick(1u << 8); assert(keys[0] == 0);
    tick(0); now += 20; tick(1u << 8); assert(keys[0] == 5);
    now += 31; button_shortcut_task(); assert(keys[0] == 0);
    tick(0); now += 300; tick(0); assert(keys[0] == 0);
    tick(1u << 8); tick(0); now += 251; tick(0); assert(keys[0] == 4);
    now += 31; button_shortcut_task(); assert(keys[0] == 0);
    // A configured chord still claims a lone tap until that button is released.
    button_shortcut_reset(); button_shortcut_task();
    slot(0, 8, 0, 4, 0); slot(1, 8, 0, 5, 0);
    config.shortcuts[1].trigger_b = 9;
    tick(1u << 8); assert(keys[0] == 0);
    tick((1u << 8) | (1u << 9)); assert(keys[0] == 5);
    now += 31; button_shortcut_task(); tick(0); assert(keys[0] == 0);
    slot(0, 8, 0, 4);
    auto invalid = config.shortcuts[0];
    invalid.flags |= SHORTCUT_FLAG_DOUBLE_TAP; assert(!shortcut_slot_valid(invalid));
    invalid = config.shortcuts[0]; invalid.action = ShortcutActionConsumer;
    assert(!shortcut_slot_valid(invalid));
}
