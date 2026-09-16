//
// Created by awalol on 2026/3/4.
//

#include "port/port.h"
#include "tusb.h"
#include <cstdio>
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

port::CriticalSection report_cs;
volatile bool report_dirty = false;

void PORT_FAST_FUNC(interrupt_loop)() {
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


    report_cs.enter();
    if (report_dirty) {
        memcpy(safe_report, interrupt_in_data, 63);
        report_dirty = false;
        should_send = true;
    }
    report_cs.exit();

    // Only send to TinyUSB if we actually grabbed fresh data
    if (should_send) {
        if (!tud_hid_report(0x01, safe_report, 63)) {
            printf("[USBHID] tud_hid_report error\n");

            // If the report failed to queue, restore the dirty flag 
            // so we try again on the next loop iteration.
            report_cs.enter();
            report_dirty = true;
            report_cs.exit();
        }
    }
}

void PORT_FAST_FUNC(on_bt_data)(CHANNEL_TYPE channel, uint8_t *data, uint16_t len) {
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

        if (get_config().polling_rate_mode != 2) {
            memcpy(interrupt_in_data, data + 3, 63);
#if ENABLE_BATT_LED
            battery_led_note_report();
#endif
            return;
        }

        // We add the critical section here to avoid any race conditions when writing to the interrupt_in_data buffer,
        // which is shared between the main loop and this callback.
        // The critical section ensures that only one thread can access the buffer at a time,
        // preventing data corruption and ensuring thread safety.
        // We also set the report_dirty flag to true to indicate that new data is available
        //  and needs to be sent in the next interrupt report.
        report_cs.enter();
        memcpy(interrupt_in_data, data + 3, 63);
        report_dirty = true;
        report_cs.exit();
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
                if (state.AllowLedColor) {
                    bt_note_host_led(state.LedRed, state.LedGreen, state.LedBlue);
                }
#if defined(DS5_FEATURE_TRACE)
                // Quiet by design: only report when the host actually asks for a
                // lightbar change, so rumble and trigger traffic does not drown
                // the log. This answers whether Steam's colour changes reach us
                // at all after a reconnect, which splits "the host stopped
                // sending" from "the controller stopped listening".
                {
                    static uint8_t last_r = 0, last_g = 0, last_b = 0;
                    static bool seen = false;
                    if (state.AllowLedColor || state.ResetLights ||
                        !seen || state.LedRed != last_r || state.LedGreen != last_g ||
                        state.LedBlue != last_b) {
                        printf("[%lu] [LED] host report: allow=%u reset=%u rgb=%02X%02X%02X fade=%u bright=%u\n",
                               (unsigned long) port::now_ms(), (unsigned) state.AllowLedColor,
                               (unsigned) state.ResetLights,
                               state.LedRed, state.LedGreen, state.LedBlue,
                               (unsigned) state.LightFadeAnimation, (unsigned) state.LightBrightness);
                        last_r = state.LedRed; last_g = state.LedGreen; last_b = state.LedBlue;
                        seen = true;
                    }
                }
#endif
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
    port::clocks_init();

    port::board_init();
    tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = static_cast<tusb_speed_t>(port::kUsbSpeed)
    };
    tusb_init(port::kUsbRhPort, &dev_init);
#if !ENABLE_SERIAL
    port::delay_ms(150);
    tud_disconnect();
#endif
    port::board_init_after_tusb();
#if ENABLE_SERIAL
    stdio_usb_init();
    while (!stdio_usb_connected()) {
        tud_task();
    }
    port::delay_ms(150);
#endif

    if (!port::bt_transport_init()) {
        printf("Failed to initialize BT transport\n");
        return 1;
    }
    // Must precede any led_set(): on ESP32-S31 this creates the RMT handle for
    // the addressable LED, without which led_set() silently does nothing. The
    // Pico backend only needs it to drive the pin low, which is what the
    // following call did on its own before.
    port::led_init();
    port::led_set(false);

    port::smps_force_pwm();

#if ENABLE_BATT_LED
    battery_led_init();
#endif

#if !ENABLE_SERIAL
    if (port::watchdog_caused_reboot()) {
        printf("Rebooted by Watchdog!\n");
        // 当崩溃重启以后，闪三下灯
        for (int i = 0; i < 6; i++) {
            if (i % 2 == 0) {
                port::led_set(true);
            } else {
                port::led_set(false);
            }
            port::delay_ms(500);
        }
    } else {
        printf("Clean boot\n");
    }
#endif

    // Initialize the critical section for the report buffer
    report_cs.init();
    wake_init();

    config_load();
    gpio_on_disconnect();

    bt_init();
    bt_register_data_callback(on_bt_data);

    audio_init();

#if !ENABLE_SERIAL
    port::watchdog_enable(1000);
#endif

    while (1) {
#if !ENABLE_SERIAL
        port::watchdog_update();
#endif
        port::bt_transport_poll();
        bt_task();
        tud_task();
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
