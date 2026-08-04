//
// Created by awalol on 2026/3/4.
//

#include <cstdio>
#include <algorithm>
#include "bsp/board_api.h"
#include "bt.h"
#include "button_functions.h"
#include "utils.h"
#include "resample.h"
#include "audio.h"
#include "btstack_util.h"
#if ENABLE_DEBUG
#include "debug.h"
#endif
#include "wake.h"
#ifdef ENABLE_WAKE_HID
#include "ps_shortcut.h"
#endif
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#if ENABLE_SERIAL
#include "pico/stdio_usb.h"
#endif
#include "config.h"
#include "cmd.h"
#include "dse.h"
#include "status_gpio.h"
#if ENABLE_BATT_LED
#include "battery_led.h"
#endif

// Pico SDK speciifically for waiting on conditions
#include "pico/critical_section.h"
#include "pico/util/queue.h"

uint8_t reportSeqCounter = 0;
uint8_t packetCounter = 0;
bool spk_active = false;

uint8_t interrupt_in_data[63] = {
    0x7f, 0x7d, 0x7f, 0x7e, 0x00, 0x00, 0xa7,
    0x08, 0x00, 0x00, 0x00, 0x52, 0x43, 0x30, 0x41,
    0x01, 0x00, 0x0e, 0x00, 0xef, 0xff, 0x03, 0x03,
    0x7b, 0x1b, 0x18, 0xf0, 0xcc, 0x9c, 0x60, 0x00,
    0xfc, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x09, 0x09, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xa7, 0xad, 0x60, 0x00, 0x29, 0x18, 0x00,
    0x53, 0x9f, 0x28, 0x35, 0xa5, 0xa8, 0x0c, 0x8b
};

critical_section_t report_cs;
volatile bool report_dirty = false;

static bool handle_output_report(const uint8_t *data, const uint16_t size) {
    // A standard DualSense USB report 0x02 contains 47 state bytes, while
    // SetStateData also covers the controller's larger 63-byte internal/Edge
    // state. Accept the complete USB payload and zero-initialize the fields
    // which are not present instead of dropping every standard output report.
    constexpr uint16_t kUsbSetStatePayloadSize = 47;
    if (size < kUsbSetStatePayloadSize) {
        return false;
    }

    SetStateData state{};
    memcpy(&state, data, std::min<uint16_t>(size, sizeof(state)));

    const auto &config = get_config();
    if (config.trigger_reduce > 0) {
        state.AllowMotorPowerLevel = 1;
        state.TriggerMotorPowerReduction = config.trigger_reduce;
    }
    if (config.speaker_gain > 0) {
        state.AllowAudioControl2 = 1;
        state.SpeakerCompPreGain = config.speaker_gain;
    }
    if (config.mic_select != 0) {
        state.AllowAudioControl = 1;
        state.MicSelect = config.mic_select;
    }
    if (config.lock_volume) {
        state.AllowHeadphoneVolume = 0;
        state.AllowMicVolume = 0;
        state.AllowSpeakerVolume = 0;
        state.AllowAudioMute = 0;
        state.AllowMuteLight = 0;
    }

    uint8_t output_data[78]{};
    output_data[0] = 0x31;
    output_data[1] = static_cast<uint8_t>(reportSeqCounter << 4);
    reportSeqCounter = static_cast<uint8_t>((reportSeqCounter + 1) & 0x0f);
    output_data[2] = 0x10;
    memcpy(output_data + 3, &state, sizeof(state));
    bt_write(output_data, sizeof(output_data));
#if ENABLE_VERBOSE
    printf_hexdump(output_data, sizeof(output_data));
#endif
    return true;
}

#if defined(DS5_WAVESHARE_STABLE_RUNTIME)
struct UsbSetReportWork {
    uint8_t report_id;
    hid_report_type_t report_type;
    uint8_t size;
    uint8_t data[64];
};

queue_t usb_set_report_queue;
alignas(4) uint8_t usb_input_tx_buffer[63];

static void usb_queues_init_before_tusb() {
    queue_init(&usb_set_report_queue, sizeof(UsbSetReportWork), 4);
}

static void queue_usb_set_report(const uint8_t report_id,
                                 const hid_report_type_t report_type,
                                 const uint8_t *data,
                                 const uint16_t size) {
    UsbSetReportWork work{};
    work.report_id = report_id;
    work.report_type = report_type;
    work.size = static_cast<uint8_t>(
        std::min<uint16_t>(size, sizeof(work.data)));
    memcpy(work.data, data, work.size);

    if (!queue_try_add(&usb_set_report_queue, &work)) {
        UsbSetReportWork stale{};
        queue_try_remove(&usb_set_report_queue, &stale);
        queue_try_add(&usb_set_report_queue, &work);
    }
}

static void usb_set_report_task() {
    UsbSetReportWork work{};
    while (queue_try_remove(&usb_set_report_queue, &work)) {
        if (is_pico_cmd(work.report_id)) {
            pico_cmd_set(work.report_id, work.data, work.size);
            continue;
        }

        if (work.report_type == HID_REPORT_TYPE_OUTPUT &&
            work.report_id == 0x02) {
            handle_output_report(work.data, work.size);
            continue;
        }

        if (work.report_type == HID_REPORT_TYPE_FEATURE &&
            (work.report_id == 0x80 || work.report_id == 0x60 ||
             work.report_id == 0x62 || work.report_id == 0x61)) {
            set_feature_data(work.report_id, work.data, work.size);
        }
    }
}
#endif

void __not_in_flash_func(interrupt_loop)() {
#if defined(DS5_WAVESHARE_STABLE_RUNTIME)
    if (!tud_mounted() || tud_suspended() || !tud_hid_ready()) {
        return;
    }

    const uint8_t polling_mode = get_config().polling_rate_mode;
    const uint32_t now = to_ms_since_boot(get_absolute_time());
    static uint32_t next_send_ms = 0;
    if (polling_mode != 2 && static_cast<int32_t>(now - next_send_ms) < 0) {
        return;
    }

    bool should_send = polling_mode != 2;
    critical_section_enter_blocking(&report_cs);
    if (report_dirty || polling_mode != 2) {
        memcpy(usb_input_tx_buffer, interrupt_in_data, sizeof(usb_input_tx_buffer));
        should_send = true;
        report_dirty = false;
    }
    critical_section_exit(&report_cs);

    if (!should_send) {
        return;
    }
    if (tud_hid_report(0x01, usb_input_tx_buffer,
                       sizeof(usb_input_tx_buffer))) {
        if (polling_mode != 2) {
            next_send_ms = now + (polling_mode == 1 ? 2u : 4u);
        }
    } else {
        critical_section_enter_blocking(&report_cs);
        report_dirty = true;
        critical_section_exit(&report_cs);
    }
    return;
#else
    if (!tud_hid_ready()) return;

    // TODO: Refactor for better code reuse
    if (get_config().polling_rate_mode != 2) {
        if (!tud_hid_report(0x01, interrupt_in_data, 63)) {
            printf("[USBHID] tud_hid_report error\n");
        }
        return;
    }

    bool should_send = false;
    // Local buffer to hold the report data while we prepare it to send. 
    uint8_t safe_report[63];


    critical_section_enter_blocking(&report_cs);
    if (report_dirty) {
        memcpy(safe_report, interrupt_in_data, 63);
        report_dirty = false;
        should_send = true;
    }
    critical_section_exit(&report_cs);

    // Only send to TinyUSB if we actually grabbed fresh data
    if (should_send) {
        if (!tud_hid_report(0x01, safe_report, 63)) {
            printf("[USBHID] tud_hid_report error\n");

            // If the report failed to queue, restore the dirty flag 
            // so we try again on the next loop iteration.
            critical_section_enter_blocking(&report_cs);
            report_dirty = true;
            critical_section_exit(&report_cs);
        }
    }
#endif
}

void __not_in_flash_func(on_bt_data)(CHANNEL_TYPE channel, uint8_t *data, uint16_t len) {
    // printf("[Main] BT data callback: channel=%u len=%u\n", channel, len);
    if (channel == INTERRUPT && len >= 66 && data[1] == 0x31) {
        // Mic audio: controller signals mic payload via bit1 of data[2];
        // the opus-encoded mic frame starts at data+4.
        if ((data[2] >> 1) & 1) {
            if (len >= 4) {
                mic_add_queue(data + 4, len - 4);
            }
            return;
        }
        if ((data[56] & 1) != (interrupt_in_data[53] & 1)) {
            set_headset(data[56] & 1);
        }
        if (((data[56] >> 2) & 1) != ((interrupt_in_data[53] >> 2) & 1)) {
            const SetStateData state{
                .AllowMuteLight = 1,
                .MuteLightMode = ((data[56] >> 2) & 1) ? MuteLight::On : MuteLight::Off,
            };
            update_state(state);
        }
        /*if (((data[12] >> 2) & 1) != ((interrupt_in_data[9] >> 2) & 1)) {
            // 如果开启了扬声器静音，这时候再按下麦克风静音，会导致扬声器静音接触。实测有线连接 DS5 也会有这个 bug
            // 有 bug，会导致游戏设置与固件设置冲突。但是实测有线连接在游戏外也不支持开关静音，先不做了。
            const SetStateData state{
                .AllowAudioMute = 1,
                .MicMute = !((interrupt_in_data[56] >> 2) & 1),
            };
            update_state(state);
        }*/

        // Wake-on-PS must observe every BT input report regardless of polling
        // mode: the wake feature has its own state to maintain (button-byte
        // diff for edge detection) and short-circuiting it on non-2 polling
        // modes silently breaks wake while the host is suspended.
        wake_on_bt_input(data + 3, len - 3);
        #ifdef ENABLE_WAKE_HID
        ps_shortcut_tick(data + 3, len - 3);
        #endif

#if !defined(DS5_WAVESHARE_STABLE_RUNTIME)
        if (get_config().polling_rate_mode != 2) {
            memcpy(interrupt_in_data, data + 3, 63);
#if ENABLE_BATT_LED
            battery_led_note_report();
#endif
            return;
        }
#endif

        // We add the critical section here to avoid any race conditions when writing to the interrupt_in_data buffer,
        // which is shared between the main loop and this callback.
        // The critical section ensures that only one thread can access the buffer at a time,
        // preventing data corruption and ensuring thread safety.
        // We also set the report_dirty flag to true to indicate that new data is available
        //  and needs to be sent in the next interrupt report.
        critical_section_enter_blocking(&report_cs);
        memcpy(interrupt_in_data, data + 3, 63);
        report_dirty = true;
        critical_section_exit(&report_cs);
#if ENABLE_BATT_LED
        battery_led_note_report();
#endif
    }
}

// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request
uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen) {
#ifdef ENABLE_WAKE_HID
    if (itf == 1) {
        if (reqlen >= 8) {
            memset(buffer, 0, 8);
            return 8;
        }
        return 0;
    }
#endif
    (void) itf;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) reqlen;

    if (is_pico_cmd(report_id)) {
        return pico_cmd_get(report_id, buffer, reqlen);
    }

#if defined(DS5_WAVESHARE_STABLE_RUNTIME)
    if (report_type == HID_REPORT_TYPE_FEATURE) {
        // Edge profiles are prefetched by dse_task(). NAK until the bounded
        // main-loop job finishes instead of returning a cacheable zero page.
        if (dse_is_profile_report(report_id) && !dse_profiles_ready()) {
            return 0;
        }
        return bt_copy_cached_feature(report_id, buffer, reqlen);
    }
#endif

    // DSE profiles: while the unlock + prefetch is still in progress, return 0
    // (NAK) for profile reads so the PS app retries rather than caching an
    // empty snapshot. Still kick off the background BT fetch.
    if (dse_is_profile_report(report_id) && !dse_profiles_ready()) {
        get_feature_data(report_id, reqlen);
        return 0;
    }

    std::vector<uint8_t> feature_data = get_feature_data(report_id, reqlen);
    if (!feature_data.empty()) {
        memcpy(buffer, feature_data.data(), feature_data.size());
    }

    return feature_data.empty() ? 0 : feature_data.size();
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void) rhport;
    uint8_t const itf = tu_u16_low(p_request->wIndex); // wInterface
    uint8_t const alt = tu_u16_low(p_request->wValue); // bAlternateSetting

    if (itf == 1) {
        printf("[AUDIO] Set interface Speaker to alternate setting %d\n", alt);
        spk_active = alt;
    }
    if (itf == 2) { // ITF_NUM_AUDIO_STREAMING_IN (microphone)
        printf("[AUDIO] Set interface Microphone to alternate setting %d\n", alt);
        set_mic_active(alt);
    }

    return true;
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize) {
#ifdef ENABLE_WAKE_HID
    if (itf == 1) {
        // Drop keyboard SET_REPORT (host LED state).
        return;
    }
#endif
#if defined(DS5_WAVESHARE_STABLE_RUNTIME)
    if (itf != 0) {
        return;
    }

    // Interrupt OUT retains report ID 0x02 in the buffer; control SET_REPORT
    // supplies it separately. Only copy here: state, heap and Bluetooth work
    // must run later in the main loop, outside the USB callback.
    if (report_type == HID_REPORT_TYPE_OUTPUT &&
        report_id == 0 && bufsize > 1 && buffer[0] == 0x02) {
        queue_usb_set_report(0x02, report_type, buffer + 1, bufsize - 1);
        return;
    }
    queue_usb_set_report(report_id, report_type, buffer, bufsize);
    return;
#else
    (void) itf;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) bufsize;

    if (is_pico_cmd(report_id)) {
#if ENABLE_VERBOSE
        printf("[HID] Receive 0xf6 setting config, funcid:0x%02X\n", buffer[0]);
#endif
        pico_cmd_set(report_id, buffer, bufsize);
        return;
    }

    // INTERRUPT OUT
    if (report_id == 0) {
        switch (buffer[0]) {
            case 0x02: {
                handle_output_report(buffer + 1, bufsize - 1);
                break;
            }
        }
    }
    if (report_id == 0x80 ||
        // DSE: Write Profile Block
        report_id == 0x60 ||
        report_id == 0x62 ||
        report_id == 0x61) {
        // set_feature_data(report_id, const_cast<uint8_t *>(buffer), bufsize);
    }
#endif
}

int main() {
#if SYS_CLOCK_KHZ != 150000
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(1000);
    set_sys_clock_khz(SYS_CLOCK_KHZ, true);
#endif

    board_init();
#if defined(DS5_WAVESHARE_STABLE_RUNTIME)
    // Windows can issue GET/SET requests immediately after tusb_init().
    critical_section_init(&report_cs);
    bt_feature_cache_init_before_tusb();
    usb_queues_init_before_tusb();
#endif
    tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_FULL
    };
    tusb_init(BOARD_TUD_RHPORT, &dev_init);
#if !ENABLE_SERIAL && !defined(DS5_WAVESHARE_STABLE_RUNTIME)
    sleep_ms(150);
    tud_disconnect();
#endif
    board_init_after_tusb();
#if ENABLE_SERIAL
    stdio_usb_init();
    while (!stdio_usb_connected()) {
        tud_task();
    }
    sleep_ms(150);
#endif

    if (cyw43_arch_init()) {
        printf("Failed to initialize CYW43\n");
        return 1;
    }
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);

#if ENABLE_BATT_LED
    battery_led_init();
#endif

#if !ENABLE_SERIAL && !defined(DS5_WAVESHARE_STABLE_RUNTIME)
    if (watchdog_caused_reboot()) {
        printf("Rebooted by Watchdog!\n");
        // 当崩溃重启以后，闪三下灯
        for (int i = 0; i < 6; i++) {
            if (i % 2 == 0) {
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
            } else {
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
            }
            sleep_ms(500);
        }
    } else {
        printf("Clean boot\n");
    }
#endif

    // Initialize the critical section for the report buffer
#if !defined(DS5_WAVESHARE_STABLE_RUNTIME)
    critical_section_init(&report_cs);
#endif
    wake_init();

    config_load();
    gpio_on_disconnect();

    bt_init();
    bt_register_data_callback(on_bt_data);

    audio_init();

#if !ENABLE_SERIAL && !defined(DS5_WAVESHARE_STABLE_RUNTIME)
    watchdog_enable(1000, true);
#endif

    while (1) {
#if !ENABLE_SERIAL && !defined(DS5_WAVESHARE_STABLE_RUNTIME)
        watchdog_update();
#endif
        cyw43_arch_poll();
        tud_task();
#if defined(DS5_WAVESHARE_STABLE_RUNTIME)
        usb_set_report_task();
#endif
        wake_task();
        audio_loop();
#if ENABLE_DEBUG
        debug_log_core1_stack_usage();
#endif
        interrupt_loop();
#if ENABLE_BATT_LED
        battery_led_tick();
#endif
        button_check();
        bt_inquiring_led();
        dse_task();
    }
}
