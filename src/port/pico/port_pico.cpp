// RP2040/RP2350 backend for the platform abstraction layer.

#include "../port.h"

#include <cstdio>
#include <cstring>

#include "bsp/board_api.h"

#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "pico/btstack_flash_bank.h"
#include "hardware/gpio.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/cyw43_arch.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/time.h"

namespace port {

// ---------------------------------------------------------------- config store

// The config lives in the last sector of flash. Keep this in step with the
// ESP32-S31 backend's partition entry.
// Upstream #244: the config used to live in the very last flash sector, which
// collides with BTstack's link-key bank. It now sits just below that bank.
static constexpr uint32_t kConfigOffset = PICO_FLASH_BANK_STORAGE_OFFSET - kConfigStorageSize;
static constexpr uint32_t kLegacyConfigOffset = PICO_FLASH_SIZE_BYTES - kConfigStorageSize;

static_assert(kConfigOffset % kConfigStorageSize == 0,
              "config region must be sector aligned");

const void *config_storage_read() {
    return reinterpret_cast<const void *>(XIP_BASE + kConfigOffset);
}

const void *config_storage_read_legacy() {
    return reinterpret_cast<const void *>(XIP_BASE + kLegacyConfigOffset);
}

// Runs with core1 parked (flash_safe_execute) and core0 interrupts disabled, so
// neither core touches XIP flash while the sector is erased/programmed. Without
// the core1 park this races the audio core and corrupts audio (buzzing).
static void config_flash_op(void *param) {
    const uint8_t *page = static_cast<const uint8_t *>(param);
    const uint32_t interrupts = save_and_disable_interrupts();
    flash_range_erase(kConfigOffset, kConfigStorageSize);
    flash_range_program(kConfigOffset, page, kConfigStoragePageSize);
    restore_interrupts(interrupts);
}

bool config_storage_write(const void *data, size_t len) {
    if (len > kConfigStoragePageSize) return false;

    alignas(4) uint8_t page[kConfigStoragePageSize];
    memset(page, 0xff, sizeof(page));
    memcpy(page, data, len);

    const int rc = flash_safe_execute(config_flash_op, page, 1000);
    if (rc != PICO_OK) {
        printf("[Port] config_storage_write flash_safe_execute failed: %d\n", rc);
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ boot button

// Read BOOTSEL by briefly floating the QSPI CSn line. Must run with both cores
// in a known-safe state - if core 1 does an XIP read while CSn is floating it
// gets garbage and the CYW43 driver misbehaves (immediate BT disconnect on audio
// start). flash_safe_execute() handles the multicore coordination - the same SDK
// mechanism BTstack uses for its TLV flash writes.
static void __no_inline_not_in_flash_func(button_read_cb)(void *param) {
    bool *out = (bool *) param;
    const uint CS_PIN_INDEX = 1;

    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    for (volatile int i = 0; i < 1000; ++i);

#if PICO_RP2350
    *out = !(sio_hw->gpio_hi_in & SIO_GPIO_HI_IN_QSPI_CSN_BITS);
#else
    *out = !(sio_hw->gpio_hi_in & (1u << CS_PIN_INDEX));
#endif

    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
}

bool boot_button_pressed() {
    bool pressed = false;
    // 100 ms timeout for the safe-state coordination; the poll is gated on the
    // 10 Hz cadence and the safe-execute parks core1, so this never runs during
    // active audio reads anyway.
    const int rc = flash_safe_execute(button_read_cb, &pressed, 100);
    if (rc != PICO_OK) return false;
    return pressed;
}

void worker_flash_safe_init() { flash_safe_execute_core_init(); }

// ---------------------------------------------------------------------- system

void clocks_init() {
#if SYS_CLOCK_KHZ != 150000
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(1000);
    set_sys_clock_khz(SYS_CLOCK_KHZ, true);
#endif
}

void smps_force_pwm() {
#ifdef CYW43_WL_GPIO_SMPS_PIN
    cyw43_arch_gpio_put(CYW43_WL_GPIO_SMPS_PIN, true);
#endif
}

bool watchdog_caused_reboot() { return ::watchdog_caused_reboot(); }
void watchdog_enable(uint32_t timeout_ms) { ::watchdog_enable(timeout_ms, true); }
void watchdog_update() { ::watchdog_update(); }

[[noreturn]] void reboot() {
    // A Cortex-M33 SYSRESETREQ does a warm reset that re-runs the flash app; a
    // watchdog reset instead drops the RP2350 bootrom into BOOTSEL, which is
    // why watchdog_reboot/enable bricked.
    *((volatile uint32_t *) 0xe000ed0c) = 0x05fa0004; // SCB AIRCR: VECTKEY | SYSRESETREQ
    __dsb();
    while (true) { tight_loop_contents(); } // wait for the reset
}

bool has_bootloader_reboot() { return true; }

[[noreturn]] void reboot_to_bootloader() {
    reset_usb_boot(0, 0); // noreturn
    __builtin_unreachable();
}

bool bt_transport_init() { return cyw43_arch_init() == 0; }
void bt_transport_poll() { cyw43_arch_poll(); }

// ------------------------------------------------------------------------ GPIO

const uint32_t kNumGpioPins = NUM_BANK0_GPIOS;

void gpio_init_output(uint32_t pin) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, false);
}

void gpio_write(uint32_t pin, bool level) { gpio_put(pin, level); }

// ------------------------------------------------------------------------- LED

void led_init() { led_set(false); }
void led_set(bool on) { cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on); }

// ----------------------------------------------------------------- one-shot timer

namespace {
// Fixed pool rather than new/delete: a cancelled alarm never reaches its
// callback, so a heap-allocated trampoline would leak on every cancel (which
// happens on each controller disconnect).
constexpr int kMaxTimers = 4;

struct TimerSlot {
    void (*cb)(void *);
    void *ctx;
    alarm_id_t alarm;     // valid only while busy
    volatile bool busy;   // owns the slot; claimed BEFORE the alarm is armed
};

TimerSlot g_slots[kMaxTimers];

// The pico alarm API hands the callback an alarm_id and expects a repeat delay
// back; the port contract is a plain one-shot, so adapt both ends here.
int64_t timer_trampoline(alarm_id_t, void *user_data) {
    auto *slot = static_cast<TimerSlot *>(user_data);
    const auto cb = slot->cb;
    void *ctx = slot->ctx;
    slot->busy = false; // release before running, so cb may re-arm
    cb(ctx);
    return 0; // do not reschedule
}
} // namespace

TimerId timer_once_ms(uint32_t delay_ms, void (*cb)(void *), void *ctx) {
    for (int i = 0; i < kMaxTimers; ++i) {
        if (g_slots[i].busy) continue;
        g_slots[i].cb = cb;
        g_slots[i].ctx = ctx;
        // Claim the slot BEFORE arming. The trampoline runs in the alarm IRQ and
        // releases the slot; if it fired between add_alarm_in_ms() returning and
        // a later store here, that store would resurrect a slot the trampoline
        // had already freed and leak it permanently. Ownership is the busy flag,
        // and alarm is only read while busy, so a stale id after release is
        // harmless.
        g_slots[i].busy = true;
        const alarm_id_t id = add_alarm_in_ms(delay_ms, timer_trampoline, &g_slots[i], false);
        if (id <= 0) {
            g_slots[i].busy = false;
            return kNoTimer;
        }
        g_slots[i].alarm = id;
        return static_cast<TimerId>(i + 1); // 0 is reserved for kNoTimer
    }
    return kNoTimer;
}

void timer_cancel(TimerId id) {
    // TimerId is int32_t, so reject <= 0 rather than just == kNoTimer: a negative
    // id passed the old `id > kMaxTimers` check and indexed g_slots[id - 1] out
    // of bounds, writing through it.
    if (id <= kNoTimer || id > kMaxTimers) return;
    TimerSlot &slot = g_slots[id - 1];
    if (!slot.busy) return; // already fired
    cancel_alarm(slot.alarm);
    slot.busy = false;
}

// ---------------------------------------------------------------------- worker

void launch_worker(void (*entry)(), uint32_t *stack, size_t stack_bytes) {
    multicore_launch_core1_with_stack(entry, stack, stack_bytes);
}

// ---------------------------------------------------------------------------- USB

const uint8_t kUsbRhPort = BOARD_TUD_RHPORT;
const uint8_t kUsbSpeed = TUSB_SPEED_FULL; // RP2350 has no high-speed PHY

void board_init() { ::board_init(); }
void board_init_after_tusb() { ::board_init_after_tusb(); }

size_t usb_get_serial(uint16_t *desc_str, size_t max_chars) {
    return board_usb_get_serial(desc_str, max_chars);
}

} // namespace port
