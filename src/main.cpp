//
// Created by awalol on 2026/3/4.
//

#include <cstdio>
#include "bsp/board_api.h"
#include "bt.h"
#include "button_functions.h"
#include "utils.h"
#include "resample.h"
#include "audio.h"
#include "btstack_util.h"
#include "button_remap.h"
#if ENABLE_DEBUG
#include "debug.h"
#endif
#include "wake.h"
#include "usb.h"
#include "button_shortcut.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
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

uint8_t reportSeqCounter = 0;
uint8_t packetCounter = 0;
bool spk_active = false;

USBGetStateData interrupt_in_data{};

bool report_dirty = false;

void __not_in_flash_func(interrupt_loop)() {
    if (usb_keyboard_only || usb_reconfiguring || !tud_hid_ready()) return;

    // TODO: Refactor for better code reuse
    if (get_config().polling_rate_mode != 2) {
        USBGetStateData report = interrupt_in_data;
        button_remap_apply(report);
        if (!tud_hid_report(0x01, &report, sizeof(report))) {
            printf("[USBHID] tud_hid_report error\n");
        }
        return;
    }

    bool should_send = false;
    USBGetStateData report{};

    if (report_dirty) {
        report = interrupt_in_data;
        report_dirty = false;
        should_send = true;
    }

    // Only send to TinyUSB if we actually grabbed fresh data
    if (should_send) {
        button_remap_apply(report);
        if (!tud_hid_report(0x01, &report, sizeof(report))) {
            printf("[USBHID] tud_hid_report error\n");

            // If the report failed to queue, restore the dirty flag 
            // so we try again on the next loop iteration.
            report_dirty = true;
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
        if ((data[56] & 1) != interrupt_in_data.PluggedHeadphones) {
            set_headset(data[56] & 1);
        }
        // Keep MuteLight in sync with the controller's current mute state,
        // primarily for handling USB Audio mute commands.
        if (((data[56] >> 2) & 1) != interrupt_in_data.MicMuted) {
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
        memcpy(&interrupt_in_data, data + 3, sizeof(interrupt_in_data));
        if (!usb_keyboard_only && !usb_reconfiguring) {
            button_shortcut_tick(interrupt_in_data);
        }

        if (get_config().polling_rate_mode != 2) {
#if ENABLE_BATT_LED
            battery_led_note_report();
#endif
            return;
        }

        report_dirty = true;
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
    (void) buffer;
    (void) bufsize;

    if (is_pico_cmd(report_id)) {
        // TinyUSB owns this buffer. USB printf() may re-enter tud_task() and reuse it,
        // so copy the entire command before printing or processing it.
        uint8_t buf_copy[CFG_TUD_HID_EP_BUFSIZE];
        memcpy(buf_copy, buffer, bufsize);

#if ENABLE_VERBOSE
        printf("[HID] Receive 0x%02X setting config, funcid:0x%02X\n", report_id, buf_copy[0]);
#endif

        pico_cmd_set(report_id, buf_copy, bufsize);
        return;
    }

    // INTERRUPT OUT
    if (report_id == 0) {
        switch (buffer[0]) {
            case 0x02: {
                uint8_t outputData[78]{};
                outputData[0] = 0x31;
                outputData[1] = reportSeqCounter << 4;
                reportSeqCounter = (reportSeqCounter + 1) & 0x0F;
                outputData[2] = 0x10;
                SetStateData state{};
                memcpy(&state,buffer + 1,sizeof(SetStateData));

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
                bt_write(outputData, sizeof(outputData));
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
#if SYS_CLOCK_KHZ != 150000
    vreg_set_voltage(VREG_VOLTAGE_1_20);
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
        wake_task();
        button_shortcut_task();
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
