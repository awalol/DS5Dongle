//
// Created by awalol on 2026/3/4.
//

#include <array>
#include <cstdio>
#include <cstring>
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
#include "usb.h"
#ifdef ENABLE_WAKE_HID
#include "ps_shortcut.h"
#endif
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
#if ENABLE_SERIAL
#include "pico/stdio_usb.h"
#endif
#include "config.h"
#include "hid_report_policy.h"
#include "battery_output_policy.h"
#include "cmd.h"
#include "dse.h"
#include "status_gpio.h"
#if ENABLE_BATT_LED
#include "battery_led.h"
#endif

// Pico SDK speciifically for waiting on conditions
#include "pico/critical_section.h"

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

void __not_in_flash_func(interrupt_loop)() {
    if (usb_keyboard_only || usb_reconfiguring) return;

    const bool endpoint_ready = tud_hid_ready();
    if (!endpoint_ready) return;
    const uint8_t mode = get_config().polling_rate_mode;

    // Local buffer to hold the report data while we prepare it to send. 
    uint8_t safe_report[ds5_hid::kInputReportBytes];


    critical_section_enter_blocking(&report_cs);
    const bool should_send = ds5_hid::copy_report_if_due(mode, endpoint_ready, report_dirty,
                                                          interrupt_in_data, safe_report);
    if (should_send && mode == 2) {
        report_dirty = false;
    }
    critical_section_exit(&report_cs);

    // Fixed modes repeat the cached state; real-time sends only new BT data.
    if (should_send) {
        if (!tud_hid_report(0x01, safe_report, ds5_hid::kInputReportBytes)) {
            printf("[USBHID] tud_hid_report error\n");

            // If the report failed to queue, restore the dirty flag 
            // so we try again on the next loop iteration.
            if (mode == 2) {
                critical_section_enter_blocking(&report_cs);
                report_dirty = true;
                critical_section_exit(&report_cs);
            }
        }
    }
}

void __not_in_flash_func(on_bt_data)(CHANNEL_TYPE channel, uint8_t *data, uint16_t len) {
    // printf("[Main] BT data callback: channel=%u len=%u\n", channel, len);
    if (channel == INTERRUPT && len > 2 && data[1] == 0x31) {
        // Mic audio: controller signals mic payload via bit1 of data[2];
        // the opus-encoded mic frame starts at data+4.
        if ((data[2] >> 1) & 1) {
            if (len >= 4) {
                mic_add_queue(data + 4, len - 4);
            }
            return;
        }
        if (!ds5_hid::complete_bt_state_report(len)) {
            // Short packets may still carry all button bytes for wake/shortcut
            // handling, but cannot replace the complete USB gamepad state.
            wake_on_bt_input(data + 3, len - 3);
#ifdef ENABLE_WAKE_HID
            if (!usb_keyboard_only && !usb_reconfiguring) {
                ps_shortcut_tick(data + 3, len - 3);
            }
#endif
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
        if (!usb_keyboard_only && !usb_reconfiguring) {
            ps_shortcut_tick(data + 3, len - 3);
        }
        #endif

        // Keep the complete BT-derived state coherent for every fixed or real-time send.
        critical_section_enter_blocking(&report_cs);
        ds5_hid::update_cached_from_bt_packet(data, len, interrupt_in_data);
        if (get_config().polling_rate_mode == 2) report_dirty = true;
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
    if (itf == usb_keyboard_instance()) {
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
    if (itf == usb_keyboard_instance()) {
        // Drop keyboard SET_REPORT (host LED state).
        return;
    }
#endif
    (void) itf;
    (void) report_id;
    (void) report_type;
    if (buffer == nullptr || bufsize == 0) return;

    if (is_pico_cmd(report_id)) {
        // USB stdio in the diagnostic log can service another control request,
        // reusing TinyUSB's SET_REPORT buffer before the command reads it.
        std::array<uint8_t, 64> command{};
        if (bufsize > command.size()) return;
        memcpy(command.data(), buffer, bufsize);
#if ENABLE_VERBOSE
        printf("[HID] Receive 0xf6 setting config, funcid:0x%02X\n", command[0]);
#endif
        pico_cmd_set(report_id, command.data(), bufsize);
        return;
    }

    // INTERRUPT OUT
    if (report_id == 0) {
        switch (buffer[0]) {
            case 0x02: {
                std::array<uint8_t, ds5_battery::wire::kStateBytes> host_state{};
                if (!ds5_battery::wire::copy_host_state(buffer, bufsize, host_state)) {
                    return;
                }
                uint8_t outputData[78]{};
                static_assert(bt_output_payload_fits(sizeof(outputData)));
                outputData[0] = 0x31;
                outputData[1] = reportSeqCounter << 4;
                reportSeqCounter = (reportSeqCounter + 1) & 0x0F;
                outputData[2] = 0x10;
                SetStateData state{};
                static_assert(sizeof(SetStateData) == ds5_battery::wire::kStateBytes);
                memcpy(&state, host_state.data(), sizeof(state));

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
                    state.NoiseCancelEnable = 1;
                }
                if (config.lock_volume) {
                    state.AllowHeadphoneVolume = 0;
                    state.AllowMicVolume = 0;
                    state.AllowSpeakerVolume = 0;
                    state.AllowAudioMute = 0;
                    state.AllowMuteLight = 0;
                }

                memcpy(outputData + 3, &state, sizeof(SetStateData));
                bt_write(outputData, sizeof(outputData), BtOutputOrigin::HostState31);
#if ENABLE_VERBOSE
                printf_hexdump(outputData,sizeof(outputData));
#endif
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
}

int main() {
#if defined(DS5_PICO2W_EXPERIMENTAL_375MHZ)
#error "The 375 MHz / 1.20 V experimental profile was retired after a reported boot failure"
#endif
#if defined(DS5_PICO2W_EXPERIMENTAL_225MHZ) && defined(DS5_PICO2W_EXPERIMENTAL_300MHZ)
#error "The Pico 2 W experimental clock profiles are mutually exclusive"
#endif
#if defined(DS5_PICO2W_EXPERIMENTAL_225MHZ) && SYS_CLOCK_KHZ != 225000
#error "The 225 MHz experimental voltage requires SYS_CLOCK_KHZ=225000"
#endif
#if defined(DS5_PICO2W_EXPERIMENTAL_300MHZ) && SYS_CLOCK_KHZ != 300000
#error "The 300 MHz experimental voltage requires SYS_CLOCK_KHZ=300000"
#endif
#if !defined(DS5_PICO2W_EXPERIMENTAL_225MHZ) && !defined(DS5_PICO2W_EXPERIMENTAL_300MHZ) && (SYS_CLOCK_KHZ == 225000 || SYS_CLOCK_KHZ == 300000 || SYS_CLOCK_KHZ == 375000)
#error "225 or 300 MHz requires its profile; 375 MHz is retired"
#endif
#if SYS_CLOCK_KHZ != 150000
#if defined(DS5_PICO2W_EXPERIMENTAL_225MHZ) || defined(DS5_PICO2W_EXPERIMENTAL_300MHZ)
    vreg_set_voltage(VREG_VOLTAGE_1_15);
#else
    vreg_set_voltage(VREG_VOLTAGE_1_20);
#endif
    sleep_ms(1000);
    set_sys_clock_khz(SYS_CLOCK_KHZ, true);
#endif

    board_init();
    config_load();
#if !ENABLE_SERIAL
    usb_keyboard_only = get_config().enable_wake;
#endif
    tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_FULL
    };
    tusb_init(BOARD_TUD_RHPORT, &dev_init);
#if !ENABLE_SERIAL
    if (!usb_keyboard_only) {
        sleep_ms(150);
        tud_disconnect();
    }
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

#ifdef CYW43_WL_GPIO_SMPS_PIN
    cyw43_arch_gpio_put(CYW43_WL_GPIO_SMPS_PIN, true);
#endif

#if ENABLE_BATT_LED
    battery_led_init();
#endif

#if !ENABLE_SERIAL
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
    critical_section_init(&report_cs);
    wake_init();

    gpio_on_disconnect();

    bt_init();
    bt_register_data_callback(on_bt_data);

    audio_init();

#if !ENABLE_SERIAL
    watchdog_enable(1000, true);
#endif

    while (1) {
#if !ENABLE_SERIAL
        watchdog_update();
#endif
        cyw43_arch_poll();
        tud_task();
        bt_battery_feedback_task();
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
