//
// Created by awalol on 2026/3/4.
//

#include "bt.h"
#include "tusb.h"
#include "bsp/board_api.h"
#include "config.h"

uint8_t mute[2]; // 0: SPEAKER(0x02) 1: MIC(0x05)
float volume[2] = {-100.0f, 0.0f}; // 0: SPEAKER(0x02) 1: MIC(0x05)

#define UAC1_ENTITY_SPK_FEATURE_UNIT    0x02
#define UAC1_ENTITY_MIC_FEATURE_UNIT    0x05

/*int main() {
    board_init();

    tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO
    };
    tusb_init(BOARD_TUD_RHPORT, &dev_init);

    board_init_after_tusb();

    while (1) {
        tud_task();
    }
}*/

//--------------------------------------------------------------------+
// Audio Callback Functions
//--------------------------------------------------------------------+

//--------------------------------------------------------------------+
// UAC1 Helper Functions
//--------------------------------------------------------------------+

static bool audio10_set_req_entity(tusb_control_request_t const *p_request, uint8_t *pBuff) {
    uint8_t channelNum = TU_U16_LOW(p_request->wValue);
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
    uint8_t entityID = TU_U16_HIGH(p_request->wIndex);
    uint8_t index = entityID == UAC1_ENTITY_SPK_FEATURE_UNIT ? 0 : 1;

    // If request is for our speaker feature unit
    if (entityID == UAC1_ENTITY_SPK_FEATURE_UNIT || entityID == UAC1_ENTITY_MIC_FEATURE_UNIT) {
        switch (ctrlSel) {
            case AUDIO10_FU_CTRL_MUTE:
                switch (p_request->bRequest) {
                    case AUDIO10_CS_REQ_SET_CUR:
                        // Only 1st form is supported
                        TU_VERIFY(p_request->wLength == 1);

                        mute[index] = pBuff[0];

                        TU_LOG2("    Set Mute: %d of entity: %u\r\n", mute[index], entityID);
                        return true;

                    default:
                        return false; // not supported
                }

            case AUDIO10_FU_CTRL_VOLUME:
                switch (p_request->bRequest) {
                    case AUDIO10_CS_REQ_SET_CUR:
                        // Only 1st form is supported
                        TU_VERIFY(p_request->wLength == 2);

                        volume[index] = static_cast<float>(*reinterpret_cast<int16_t const *>(pBuff)) / 256;
                        if (entityID == UAC1_ENTITY_SPK_FEATURE_UNIT) {
                            auto config = get_config();
                            config.speaker_volume = volume[index];
                            set_config(config);
                        }

                        TU_LOG2("    Set Volume: %d dB of entity: %u\r\n", volume[index], entityID);
                        return true;

                    default:
                        return false; // not supported
                }

            // Unknown/Unsupported control
            default:
                TU_BREAKPOINT();
                return false;
        }
    }

    return false;
}

static bool audio10_get_req_entity(uint8_t rhport, tusb_control_request_t const *p_request) {
    uint8_t channelNum = TU_U16_LOW(p_request->wValue);
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
    uint8_t entityID = TU_U16_HIGH(p_request->wIndex);
    uint8_t index = entityID == UAC1_ENTITY_SPK_FEATURE_UNIT ? 0 : 1;

    // If request is for our speaker feature unit
    if (entityID == UAC1_ENTITY_SPK_FEATURE_UNIT || entityID == UAC1_ENTITY_MIC_FEATURE_UNIT) {
        switch (ctrlSel) {
            case AUDIO10_FU_CTRL_MUTE:
                // Audio control mute cur parameter block consists of only one byte - we thus can send it right away
                // There does not exist a range parameter block for mute
                TU_LOG2("    Get Mute of entity: %u\r\n", entityID);
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &mute[index], 1);

            case AUDIO10_FU_CTRL_VOLUME:
                switch (p_request->bRequest) {
                    case AUDIO10_CS_REQ_GET_CUR:
                        TU_LOG2("    Get Volume of entity: %u\r\n", entityID); {
                            if (entityID == UAC1_ENTITY_SPK_FEATURE_UNIT) {
                                volume[index] = get_config().speaker_volume;
                            }
                            int16_t vol = volume[index] * 256; // convert to 1/256 dB units
                            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &vol, sizeof(vol));
                        }

                    case AUDIO10_CS_REQ_GET_MIN:
                        TU_LOG2("    Get Volume min of entity: %u\r\n", entityID); {
                            uint8_t min[2];
                            if (entityID == UAC1_ENTITY_SPK_FEATURE_UNIT) {
                                min[0] = 0x00;
                                min[1] = 0x9c;
                            } else {
                                min[0] = 0x00;
                                min[1] = 0x00;
                            }
                            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &min, sizeof(min));
                        }

                    case AUDIO10_CS_REQ_GET_MAX:
                        TU_LOG2("    Get Volume max of entity: %u\r\n", entityID); {
                            uint8_t max[2];
                            if (entityID == UAC1_ENTITY_SPK_FEATURE_UNIT) {
                                max[0] = 0x00;
                                max[1] = 0x00;
                            } else {
                                max[0] = 0x00;
                                max[1] = 0x30;
                            }
                            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &max, sizeof(max));
                        }

                    case AUDIO10_CS_REQ_GET_RES:
                        TU_LOG2("    Get Volume res of entity: %u\r\n", entityID); {
                            uint8_t res[2];
                            if (entityID == UAC1_ENTITY_SPK_FEATURE_UNIT) {
                                res[0] = 0x00;
                                res[1] = 0x01;
                            } else {
                                res[0] = 0x7a;
                                res[1] = 0x00;
                            }
                            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &res, sizeof(res));
                        }
                    // Unknown/Unsupported control
                    default:
                        TU_BREAKPOINT();
                        return false;
                }
                break;

            // Unknown/Unsupported control
            default:
                TU_BREAKPOINT();
                return false;
        }
    }

    return false;
}

// Invoked when audio class specific get request received for an entity
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void) rhport;

    return audio10_get_req_entity(rhport, p_request);
}

// Invoked when audio class specific set request received for an entity
bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf) {
    (void) rhport;

    return audio10_set_req_entity(p_request, buf);
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len) {
    (void) instance;
    (void) len;
}

bool sleep = false;
bool prev_sleep = false;  // Track previous sleep state to detect transitions

bool usb_is_sleep() {
    return sleep;
}

void usb_on_mount() {
    printf("[USB] invoke tud_mount_cb\n");
    sleep = false;
    if (bt_connected()) {
        return;
    }
    printf("[USB] tud_mount_cb: bt_scan_start and tud_disconnect\n");
    bt_scan_start();
    tud_disconnect();
}

void usb_on_umount() {
    // In actual testing, power-off triggers tud_suspend_cb. So far, no tud_umount_cb has been observed.
    printf("[USB] invoke tud_umount_cb\n");
    // Only disconnect when transitioning INTO sleep (not if already sleeping)
    if (!sleep) {
        sleep = true;
        if (bt_connected()) {
            bt_disconnect();
        }
    }
    bt_scan_stop();
}

void usb_on_suspend(bool remote_wakeup_en) {
    // When USB enters sleep mode, turn off the connected controller and
    // keep the USB attachment alive so the host can still wake via remote wakeup.
    (void) remote_wakeup_en;

    printf("[USB] invoke tud_suspend_cb\n");
    // Only disconnect when transitioning INTO sleep (not if already sleeping)
    if (!sleep) {
        sleep = true;
        if (bt_connected()) {
            printf("[USB] tud_suspend_cb: Disconnecting controller on sleep transition\n");
            bt_disconnect();
        }
    }
#ifdef ENABLE_WAKE_HID
    printf("[USB] tud_suspend_cb: bt_scan_start (wake HID enabled)\n");
    bt_scan_start();
#else
    printf("[USB] tud_suspend_cb: bt_scan_stop\n");
    bt_scan_stop();
#endif
}

void usb_on_resume() {
    printf("[USB] invoke tud_resume_cb\n");
    // Only process when transitioning OUT OF sleep
    if (sleep) {
        sleep = false;
        printf("[USB] tud_resume_cb: Woke from sleep\n");
    }
    if (!bt_connected()) {
        bt_scan_start();
#ifdef ENABLE_WAKE_HID
        printf("[USB] tud_resume_cb: bt_scan_start\n");
#else
        printf("[USB] tud_resume_cb: bt_scan_start and tud_disconnect\n");
        tud_disconnect();
#endif
    } else {
        printf("[USB] tud_resume_cb: already connected, reconnecting USB\n");
#if !ENABLE_SERIAL
        tud_connect();
#endif
    }
}

#ifndef ENABLE_WAKE_HID
void tud_mount_cb(void) {
    usb_on_mount();
}

void tud_umount_cb(void) {
    usb_on_umount();
}

void tud_suspend_cb(bool remote_wakeup_en) {
    usb_on_suspend(remote_wakeup_en);
}

void tud_resume_cb(void) {
    usb_on_resume();
}
#endif
