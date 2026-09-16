#pragma once

#include <cstdint>

struct USBGetStateData;

constexpr uint8_t BUTTON_SHORTCUT_COUNT = 9;
constexpr uint8_t BUTTON_SHORTCUT_DISABLED = 0xFF;
// trigger_b sentinels: tap on trigger_a alone instead of a second held button.
constexpr uint8_t SHORTCUT_TRIGGER_TAP = 0xFD;
constexpr uint8_t SHORTCUT_TRIGGER_DOUBLE_TAP = 0xFE;
// Chords have no sentinel to spare (trigger_b is the second button), so they mark
// the double tap here instead.
constexpr uint8_t SHORTCUT_FLAG_DOUBLE_TAP = 0x01;
constexpr uint8_t SHORTCUT_FLAG_HOLD = 0x02; // Keyboard follows physical state; TAP means one button.
constexpr uint8_t SHORTCUT_FLAG_MASK = SHORTCUT_FLAG_DOUBLE_TAP | SHORTCUT_FLAG_HOLD;
constexpr uint32_t SHORTCUT_TAP_WINDOW_MS = 250;

enum ShortcutAction : uint8_t {
    ShortcutActionKeyboard = 0,
    ShortcutActionBtDisconnect,
    ShortcutActionConsumer,
};

struct __attribute__((packed)) ButtonShortcut {
    uint8_t trigger_a; // ButtonId; BUTTON_SHORTCUT_DISABLED means the slot is disabled
    uint8_t trigger_b; // ButtonId (both triggers must be held), or a SHORTCUT_TRIGGER_* tap sentinel
    uint8_t action; // ShortcutAction
    union {
        struct __attribute__((packed)) {
            uint8_t modifiers;
            uint8_t key; // USB HID keyboard usage ID; 0 means no regular key
            uint8_t reserved;
        } keyboard; // ShortcutActionKeyboard
        struct __attribute__((packed)) {
            uint16_t usage; // USB HID consumer page usage ID (little-endian)
            uint8_t reserved;
        } consumer; // ShortcutActionConsumer
        uint8_t payload[3]; // Whole action payload, for zeroing
    };
    uint8_t flags; // SHORTCUT_FLAG_*; sits after the payload so its offset is stable
};

bool shortcut_slot_valid(const ButtonShortcut &shortcut);

#ifdef ENABLE_WAKE_HID
void button_shortcut_tick(const USBGetStateData& state);
void button_shortcut_reset();
void button_shortcut_task();
#else
// No keyboard/consumer HID interface to fire into: keep the call sites clean
// instead of sprinkling #ifdef through main.cpp and wake.cpp.
static inline void button_shortcut_tick(const USBGetStateData&) {}
static inline void button_shortcut_reset() {}
static inline void button_shortcut_task() {}
#endif
