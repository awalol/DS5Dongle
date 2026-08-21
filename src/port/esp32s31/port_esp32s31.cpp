// ESP32-S31 backend for the platform abstraction layer.
//
// Target board: ESP32-S31-Function-CoreBoard-1 (ESP32-S31-WROOM-3, 16MB flash,
// 16MB PSRAM). Pin assignments live in board_esp32s31.h.

#include "../port.h"
#include "board_esp32s31.h"

#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "soc/soc_caps.h"
#include "esp_mac.h"
#include "esp_partition.h"
#include "esp_private/usb_phy.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "tusb.h"

namespace port {

// ---------------------------------------------------------------- config store

namespace {
const esp_partition_t *g_config_part = nullptr;

// RAM shadow of the config region. ESP-IDF cannot erase a partition while it is
// mmap'd, so unlike the XIP-mapped RP2350 path we read into RAM and hand
// callers a pointer to that. kConfigStoragePageSize is all the caller ever
// writes, but the shadow covers what it may read back.
alignas(4) uint8_t g_config_shadow[kConfigStoragePageSize];
bool g_config_loaded = false;

const esp_partition_t *config_partition() {
    if (!g_config_part) {
        g_config_part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "config");
    }
    return g_config_part;
}
} // namespace

const void *config_storage_read() {
    if (!g_config_loaded) {
        const esp_partition_t *part = config_partition();
        if (part && esp_partition_read(part, 0, g_config_shadow, sizeof(g_config_shadow)) == ESP_OK) {
            g_config_loaded = true;
        } else {
            // Present erased flash so the caller's magic/CRC check fails and it
            // falls back to defaults, matching a blank Pico.
            memset(g_config_shadow, 0xff, sizeof(g_config_shadow));
            g_config_loaded = true;
        }
    }
    return g_config_shadow;
}

const void *config_storage_read_legacy() {
    // No legacy location: this port has always used its own "config" partition,
    // so there is nothing to migrate from.
    return nullptr;
}

bool config_storage_write(const void *data, size_t len) {
    if (len > kConfigStoragePageSize) return false;

    const esp_partition_t *part = config_partition();
    if (!part) {
        printf("[Port] config partition not found\n");
        return false;
    }

    alignas(4) uint8_t page[kConfigStoragePageSize];
    memset(page, 0xff, sizeof(page));
    memcpy(page, data, len);

    // esp_partition_erase_range takes the flash-op lock internally, which parks
    // the other core's cache access for the duration -- the equivalent of the
    // Pico build's flash_safe_execute core1 park.
    esp_err_t err = esp_partition_erase_range(part, 0, kConfigStorageSize);
    if (err != ESP_OK) {
        printf("[Port] config erase failed: %d\n", err);
        return false;
    }
    err = esp_partition_write(part, 0, page, sizeof(page));
    if (err != ESP_OK) {
        printf("[Port] config write failed: %d\n", err);
        return false;
    }

    memcpy(g_config_shadow, page, sizeof(page));
    g_config_loaded = true;
    return true;
}

// ----------------------------------------------------------------- one-shot timer

namespace {
constexpr int kMaxTimers = 4;

struct TimerSlot {
    void (*cb)(void *);
    void *ctx;
    // Created on first use and then kept forever. Never deleting the handle is
    // what makes cancel safe: esp_timer_stop() on a stale-but-live handle is
    // merely ESP_ERR_INVALID_STATE, whereas stopping a freed one corrupts the
    // esp_timer list and panics minutes later with an unrelated backtrace.
    esp_timer_handle_t handle;
    // Ownership token. Claimed by an atomic exchange so exactly one context --
    // the arming caller, the cancelling caller, or the trampoline -- ever acts
    // on a given slot. The trampoline runs on the esp_timer task (priority 22)
    // while callers are typically the main task (priority 1) on the same core,
    // so this is a genuine preemption, not a theoretical one.
    volatile bool armed;
};

TimerSlot g_slots[kMaxTimers];

void timer_trampoline(void *arg) {
    auto *slot = static_cast<TimerSlot *>(arg);
    // Release before running so the callback may re-arm, matching the Pico
    // backend. Losing this exchange means a cancel got here first.
    if (!__atomic_exchange_n(&slot->armed, false, __ATOMIC_SEQ_CST)) return;
    slot->cb(slot->ctx);
}
} // namespace

TimerId timer_once_ms(uint32_t delay_ms, void (*cb)(void *), void *ctx) {
    for (int i = 0; i < kMaxTimers; ++i) {
        bool expected = false;
        if (!__atomic_compare_exchange_n(&g_slots[i].armed, &expected, true,
                                         false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            continue; // in use
        }

        // The slot is ours now, so lazily creating the handle here is safe.
        if (g_slots[i].handle == nullptr) {
            const esp_timer_create_args_t args = {
                .callback = timer_trampoline,
                .arg = &g_slots[i],
                .dispatch_method = ESP_TIMER_TASK,
                .name = "port_oneshot",
                .skip_unhandled_events = true,
            };
            if (esp_timer_create(&args, &g_slots[i].handle) != ESP_OK) {
                __atomic_store_n(&g_slots[i].armed, false, __ATOMIC_SEQ_CST);
                return kNoTimer;
            }
        }

        // Published before the timer can possibly fire, since it is not started
        // until the next statement.
        g_slots[i].cb = cb;
        g_slots[i].ctx = ctx;

        if (esp_timer_start_once(g_slots[i].handle,
                                 static_cast<uint64_t>(delay_ms) * 1000) != ESP_OK) {
            __atomic_store_n(&g_slots[i].armed, false, __ATOMIC_SEQ_CST);
            return kNoTimer;
        }
        return static_cast<TimerId>(i + 1); // 0 is reserved for kNoTimer
    }
    return kNoTimer;
}

void timer_cancel(TimerId id) {
    // TimerId is int32_t: reject <= 0, not just == kNoTimer. A negative id
    // passed the old check and indexed g_slots[id - 1] out of bounds.
    if (id <= kNoTimer || id > kMaxTimers) return;
    TimerSlot &slot = g_slots[id - 1];
    // Losing this exchange means it already fired; there is nothing to stop and
    // the trampoline owns the slot.
    if (!__atomic_exchange_n(&slot.armed, false, __ATOMIC_SEQ_CST)) return;
    esp_timer_stop(slot.handle);
}

// ------------------------------------------------------------------ boot button

bool boot_button_pressed() {
    static bool configured = false;
    if (!configured) {
        const gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << BOARD_BOOT_BUTTON_GPIO,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
        configured = true;
    }
    // BOOT is active-low. Unlike the RP2350 QSPI-CSn trick this is a cheap read
    // with no core parking, so the 10 Hz cadence upstream is conservative here.
    return gpio_get_level(static_cast<gpio_num_t>(BOARD_BOOT_BUTTON_GPIO)) == 0;
}

void worker_flash_safe_init() {
    // No-op: esp_partition_* handles cross-core cache coordination itself, so
    // the worker core needs no registration.
}

// ---------------------------------------------------------------------- system

void clocks_init() {
    // CPU frequency comes from sdkconfig (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ) and
    // is applied by the bootloader before app_main, so there is nothing to do.
}

void smps_force_pwm() {
    // The board's regulator has no software-controlled mode pin.
}

bool watchdog_caused_reboot() {
    const esp_reset_reason_t reason = esp_reset_reason();
    return reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT || reason == ESP_RST_WDT;
}

void watchdog_enable(uint32_t timeout_ms) {
    const esp_task_wdt_config_t cfg = {
        .timeout_ms = timeout_ms,
        .idle_core_mask = 0, // the audio worker never idles; do not watch idle tasks
        .trigger_panic = true,
    };
    // Already-initialised is fine: sdkconfig may enable the TWDT at startup.
    esp_err_t err = esp_task_wdt_init(&cfg);
    if (err == ESP_ERR_INVALID_STATE) err = esp_task_wdt_reconfigure(&cfg);
    if (err != ESP_OK) {
        printf("[Port] task wdt init failed: %d\n", err);
        return;
    }
    esp_task_wdt_add(nullptr); // watch the calling (main loop) task
}

void watchdog_update() { esp_task_wdt_reset(); }

[[noreturn]] void reboot() {
    esp_restart();
    __builtin_unreachable();
}

bool has_bootloader_reboot() { return false; }

[[noreturn]] void reboot_to_bootloader() {
    // Unreachable in practice -- button_functions.cpp checks the capability
    // first -- but the port interface requires a definition, so keep it honest
    // rather than silently pretending a plain restart is a flash mode.
    printf("[Port] no software route to download boot on ESP32-S31; hold BOOT "
           "(GPIO61) while resetting, or just let esptool do it\n");
    esp_restart();
    __builtin_unreachable();
}

// bt_transport_init()/bt_transport_poll() live in bt_transport_esp32s31.cpp.

// ------------------------------------------------------------------------ GPIO

const uint32_t kNumGpioPins = SOC_GPIO_PIN_COUNT;

void gpio_init_output(uint32_t pin) {
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_write(pin, false);
}

void gpio_write(uint32_t pin, bool level) {
    gpio_set_level(static_cast<gpio_num_t>(pin), level ? 1 : 0);
}

// ------------------------------------------------------------------------- LED

namespace {
led_strip_handle_t g_led = nullptr;
} // namespace

void led_init() {
    // GPIO60 reaches the WS2812 through Q1 (LBSS138) as a level shifter to 5V.
    // That stage is NON-inverting: the gate is tied to ESP_3V3, GPIO60 drives
    // the source and RGB_CTRL is the drain with R13 pulling to VCC_5V, so
    // RGB_CTRL simply follows GPIO60. Do not set invert_out.
    const led_strip_config_t strip_cfg = {
        .strip_gpio_num = BOARD_STATUS_LED_GPIO,
        .max_leds = 1,
    };
    const led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000, // 10 MHz
    };
    if (led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &g_led) != ESP_OK) {
        printf("[Port] status LED init failed\n");
        g_led = nullptr;
        return;
    }
    led_set(false);
}

void led_set(bool on) {
    if (!g_led) return;
    if (on) {
        // Dim white: the board LED is far brighter than the Pico2W's, and this
        // interface only promises on/off.
        led_strip_set_pixel(g_led, 0, 8, 8, 8);
        led_strip_refresh(g_led);
    } else {
        led_strip_clear(g_led);
    }
}

// ---------------------------------------------------------------------------- USB

const uint8_t kUsbRhPort = 0;

// The S31's OTG has a UTMI (high-speed) PHY and no FS/LS transceiver
// (soc_caps.h: SOC_USB_FSLS_PHY_NUM 0). This target enumerates HIGH speed,
// matching the real DualSense, so PHY and requested speed agree. The RP2350 is
// full-speed only; ep_interval() encodes bInterval for whichever is built.
const uint8_t kUsbSpeed = TUSB_SPEED_HIGH;

namespace {
usb_phy_handle_t g_usb_phy = nullptr;
} // namespace

void board_init() {
    // Raw TinyUSB, not esp_tinyusb, so nothing else brings the OTG PHY up --
    // do it before tusb_init(). Device mode on the UTMI PHY is purely a
    // software choice; the Type-A receptacle does not force host role.
    //
    // otg_io_conf stays null deliberately: unset means drvvbus is never routed
    // and the SoC never drives VBUS. Board VBUS comes from a separate TPS2051C,
    // hence the VBUS-disconnected host cable. With no VBUS sense the device
    // attaches whenever powered rather than when the host appears.
    const usb_phy_config_t phy_cfg = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_UTMI,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_HIGH,
        .ext_io_conf = nullptr,
        .otg_io_conf = nullptr,
    };
    const esp_err_t err = usb_new_phy(&phy_cfg, &g_usb_phy);
    if (err != ESP_OK) {
        printf("[Port] usb_new_phy failed: %d\n", err);
        g_usb_phy = nullptr;
    }
}

namespace {

// Report the speed dcd_init() settled on. Deliberately read-only: the speed is
// settled by BOARD_TUD_MAX_SPEED in ports/esp32s31/CMakeLists.txt, and forcing
// DCFG.DevSpd here instead stopped the host enumerating the device at all.
// Worth logging, since DevSpd decides which speed the descriptors are encoded
// for -- see ep_interval() in src/usb_descriptors.cpp.
void usb_log_speed() {
    // ESP32-S31 OTG_HS base, per TinyUSB's dwc2_esp32.h controller table.
    // DCFG is at 0x800 with DevSpd in bits [1:0]:
    //   0 = high speed, 1 = full speed on HS PHY, 2 = low speed, 3 = full speed
    static const char *const kDevSpdName[] = {"high", "full (HS PHY)", "low", "full (FS PHY)"};
    constexpr uintptr_t kOtgHsBase = 0x20300000UL;
    constexpr uintptr_t kDcfgOffset = 0x800;

    // DSTS at 0x808 reports what the link actually enumerated at, in bits [2:1],
    // using the same encoding. DevSpd is what we asked for; DSTS is what we got,
    // and only the second one is evidence.
    constexpr uintptr_t kDstsOffset = 0x808;
    const uint32_t dcfg = *reinterpret_cast<volatile uint32_t *>(kOtgHsBase + kDcfgOffset);
    const uint32_t dsts = *reinterpret_cast<volatile uint32_t *>(kOtgHsBase + kDstsOffset);
    printf("[USB] DCFG %08lX DevSpd %lu (%s), DSTS %08lX enum %lu (%s)\n",
           (unsigned long) dcfg, (unsigned long) (dcfg & 0x3u), kDevSpdName[dcfg & 0x3u],
           (unsigned long) dsts, (unsigned long) ((dsts >> 1) & 0x3u),
           kDevSpdName[(dsts >> 1) & 0x3u]);
}

} // namespace

void board_init_after_tusb() {
    usb_log_speed();
}

size_t usb_get_serial(uint16_t *desc_str, size_t max_chars) {
    // Derive a stable serial from the factory MAC, mirroring what TinyUSB's BSP
    // does with the RP2350 unique flash ID.
    uint8_t mac[6] = {};
    if (esp_efuse_mac_get_default(mac) != ESP_OK) return 0;

    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (size_t i = 0; i < sizeof(mac) && n + 2 <= max_chars; ++i) {
        desc_str[n++] = static_cast<uint16_t>(hex[mac[i] >> 4]);
        desc_str[n++] = static_cast<uint16_t>(hex[mac[i] & 0x0f]);
    }
    return n;
}

// ---------------------------------------------------------------------- worker

namespace {
StaticTask_t g_worker_tcb;
void (*g_worker_entry)() = nullptr;

void worker_thunk(void *) {
    if (g_worker_entry) g_worker_entry();
    vTaskDelete(nullptr); // entry is not expected to return
}
} // namespace

void launch_worker(void (*entry)(), uint32_t *stack, size_t stack_bytes) {
    g_worker_entry = entry;
    // Static allocation from the caller's buffer, pinned to core 1 -- keeps the
    // audio worker's stack where the Pico build had it so the high-water-mark
    // instrumentation still measures the same memory.
    xTaskCreateStaticPinnedToCore(worker_thunk, "audio_core1",
                                  stack_bytes / sizeof(StackType_t),
                                  nullptr, configMAX_PRIORITIES - 2,
                                  reinterpret_cast<StackType_t *>(stack),
                                  &g_worker_tcb, 1);
}

} // namespace port
