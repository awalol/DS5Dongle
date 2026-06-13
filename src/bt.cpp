//
// Created by awalol on 2026/3/4.
//

#include <cstdio>
#include <cstring>
#include "bt.h"
#include <queue>
#include <unordered_map>
#include <vector>
#include "btstack_event.h"
#include "gap.h"
#include "l2cap.h"
#include "pico/cyw43_arch.h"
#include "utils.h"
#include "bsp/board_api.h"
#include "classic/sdp_server.h"
#include "config.h"
#include "state_mgr.h"
#include "wake.h"
#include "pico/util/queue.h"
#if ENABLE_BATT_LED
#include "battery_led.h"
#endif

#define MTU_CONTROL 672
#define MTU_INTERRUPT 672

using std::unordered_map;
using std::vector;
using std::queue;

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static void l2cap_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static btstack_packet_callback_registration_t hci_event_callback_registration, l2cap_event_callback_registration;
static bd_addr_t current_device_addr;
static bool device_found = false;
static bool new_pair = false; // 只有新匹配的设备才用创建channel，自动重连走的是service
bool bt_inquiring = false;
static hci_con_handle_t acl_handle = HCI_CON_HANDLE_INVALID;
static uint16_t hid_control_cid;
static uint16_t hid_interrupt_cid;
static bt_data_callback_t bt_data_callback = nullptr;
static bool check_dse = false;
// DSE profile unlock state: 0=idle, 1=waiting for controller to process 0x80,
// 2=profiles prefetched, waiting briefly for responses before USB connect
static int dse_unlock_phase = 0;
static uint32_t dse_unlock_started_ms = 0;
// False from DSE connect until the unlock+prefetch completes. While false,
// GET of profile reports 0x70-0x7B returns a short "busy" response so the
// PS app retries (it polls these repeatedly during load) instead of caching
// an empty snapshot. No USB enumeration delay required.
static bool dse_profiles_ready = true;

bool bt_dse_profiles_ready() { return dse_profiles_ready; }
static uint32_t dse_profile_written_ms = 0;
static int dse_profile_refresh_round = 0;
// Paced profile prefetch sequencer: fetching all 12 profile reports as a
// burst overflows the L2CAP control channel and silently drops requests
// (observed as one profile "not assigned" after reconnect). One GET per
// 80ms is reliable.
static uint8_t dse_prefetch_next = 0;      // 0 = idle, else next report id
static uint32_t dse_prefetch_last_ms = 0;
static bool dse_prefetch_connect = false;  // tud_connect() when done

static void dse_prefetch_start(bool connect_after) {
    dse_prefetch_next = 0x70;
    dse_prefetch_last_ms = 0;
    dse_prefetch_connect = connect_after;
}
static int8_t bt_rssi = 0;
unordered_map<uint8_t, vector<uint8_t> > feature_data;
queue_t send_fifo;

struct send_element {
    uint8_t data[512];
    size_t len;
};

absolute_time_t inactive_time = 0; // 手柄长时间静默

void bt_register_data_callback(bt_data_callback_t callback) {
    bt_data_callback = callback;
}

void bt_send_packet(uint8_t *data, uint16_t len) {
    if (hid_interrupt_cid != 0) {
        l2cap_send(hid_interrupt_cid, data, len);
    }
}

void bt_send_control(uint8_t *data, uint16_t len) {
    if (hid_control_cid != 0) {
        l2cap_send(hid_control_cid, data, len);
    }
}

bool bt_disconnect() {
    if (acl_handle == HCI_CON_HANDLE_INVALID) {
        return false;
    }

    // 0x13 = remote user terminated connection
    hci_send_cmd(&hci_disconnect, acl_handle, 0x13);
    return true;
}

bool bt_is_connected() {
    return hid_interrupt_cid != 0;
}

void bt_set_scan_idle() {
    gap_set_page_scan_activity(0x1000, 0x0012);
    gap_inquiry_set_scan_activity(0x1000, 0x0012);
}

void bt_set_scan_active() {
    gap_set_page_scan_activity(0x0800, 0x0012);
    gap_inquiry_set_scan_activity(0x0800, 0x0012);
}

void bt_get_signal_strength(int8_t *rssi) {
    // gap_read_rssi() completes asynchronously, so this function can only
    // return the last cached RSSI value. Trigger a refresh afterwards so a
    // subsequent call can observe the updated value once the RSSI event arrives.
    if (rssi != nullptr) {
        *rssi = bt_rssi;
    }
    if (acl_handle != HCI_CON_HANDLE_INVALID) {
        gap_read_rssi(acl_handle);
    }
}

void bt_l2cap_init() {
    l2cap_event_callback_registration.callback = &l2cap_packet_handler;
    l2cap_add_event_handler(&l2cap_event_callback_registration);
    // 修复重连后自动断开的关键点
    sdp_init();
    l2cap_register_service(l2cap_packet_handler, PSM_HID_CONTROL, MTU_CONTROL, LEVEL_2);
    l2cap_register_service(l2cap_packet_handler, PSM_HID_INTERRUPT, MTU_INTERRUPT, LEVEL_2);

    l2cap_init();
}

int bt_init() {
    queue_init(&send_fifo, sizeof(send_element), 10);

    bt_l2cap_init();

    // SSP (Secure Simple Pairing)
    gap_ssp_set_enable(true);
    gap_secure_connections_enable(true);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_YES_NO);
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);

    gap_connectable_control(1);
    gap_discoverable_control(1);

    hci_event_callback_registration.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    hci_power_control(HCI_POWER_ON);
    return 0;
}

/*int main() {
    stdio_init_all();

    /*while (!stdio_usb_connected()) {
        sleep_ms(100);
    }
    printf("USB Serial connected!\n");#1#

    bt_init();

    while (1) {
        sleep_ms(10);
    }
}*/

void bt_inquiring_led() {
    if (hid_interrupt_cid != 0) {
        return;
    }
    static bool led_status = false;
    if (!bt_inquiring) {
        if (led_status) {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
        }
        return;
    }
    static auto last_time = time_us_32();
    if (time_us_32() - last_time > 200 * 1000) {
        last_time = time_us_32();
        led_status = !led_status;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_status);
    }
}

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void) channel;

    const uint8_t event_type = hci_event_packet_get_type(packet);

    switch (event_type) {
        case BTSTACK_EVENT_STATE: {
            const uint8_t state = btstack_event_state_get_state(packet);
            printf("[BT] State: %u\n", state);
            if (state == HCI_STATE_WORKING) {
                printf("[BT] Stack ready, start inquiry\n");
                gap_inquiry_start(30);
                bt_inquiring = true;
            }
            break;
        }
        case HCI_EVENT_INQUIRY_RESULT:
        case HCI_EVENT_INQUIRY_RESULT_WITH_RSSI:
        case HCI_EVENT_EXTENDED_INQUIRY_RESPONSE: {
            bd_addr_t addr;
            uint32_t cod;

            if (event_type == HCI_EVENT_INQUIRY_RESULT) {
                cod = hci_event_inquiry_result_get_class_of_device(packet);
                hci_event_inquiry_result_get_bd_addr(packet, addr);
            } else if (event_type == HCI_EVENT_INQUIRY_RESULT_WITH_RSSI) {
                cod = hci_event_inquiry_result_with_rssi_get_class_of_device(packet);
                hci_event_inquiry_result_with_rssi_get_bd_addr(packet, addr);
            } else {
                cod = hci_event_extended_inquiry_response_get_class_of_device(packet);
                hci_event_extended_inquiry_response_get_bd_addr(packet, addr);
            }

            // CoD 0x002508 = Gamepad (Major: Peripheral, Minor: Gamepad)
            if ((cod & 0x000F00) == 0x000500) {
                printf("[HCI] Gamepad found: %s (CoD: 0x%06x)\n", bd_addr_to_str(addr), (unsigned int) cod);
                bd_addr_copy(current_device_addr, addr);
                device_found = true;
                gap_inquiry_stop();
            }
            break;
        }
        case GAP_EVENT_INQUIRY_COMPLETE:
        case HCI_EVENT_INQUIRY_COMPLETE: {
            printf("[HCI] Inquiry complete.\n");
            bt_inquiring = false;
            if (device_found) {
                printf("[HCI] Connecting to %s...\n", bd_addr_to_str(current_device_addr));
                new_pair = true;
                hci_send_cmd(&hci_create_connection, current_device_addr,
                             hci_usable_acl_packet_types(), 0, 0, 0, 1);
                break;
            }
            if (event_type == HCI_EVENT_INQUIRY_COMPLETE) {
                // gap_inquiry_start(30);
                gap_connectable_control(1);
                gap_discoverable_control(1);
            }
            break;
        }
        case HCI_EVENT_COMMAND_STATUS: {
            const uint8_t status = hci_event_command_status_get_status(packet);
            const uint16_t opcode = hci_event_command_status_get_command_opcode(packet);
            printf("[HCI] CmdStatus %s(0x%04X) status=0x%02X\n", opcode_to_str(opcode), opcode, status);
            if (opcode == HCI_OPCODE_HCI_CREATE_CONNECTION && status != ERROR_CODE_SUCCESS) {
                device_found = false;
                new_pair = false;
                printf("[HCI] Create connection rejected\n");
                // gap_inquiry_start(30);
            }
            if (opcode == HCI_OPCODE_HCI_INQUIRY_CANCEL) {
                bt_inquiring = false;
            }
            break;
        }

        case HCI_EVENT_COMMAND_COMPLETE: {
            const uint8_t status = hci_event_command_complete_get_return_parameters(packet)[0];
            const uint16_t opcode = hci_event_command_complete_get_command_opcode(packet);
            if (opcode != HCI_OPCODE_HCI_READ_RSSI) {
                printf("[HCI] CmdComplete %s(0x%04X) status=0x%02X\n", opcode_to_str(opcode), opcode, status);
            }
            if (opcode == HCI_OPCODE_HCI_READ_RSSI) {
                if (status != ERROR_CODE_SUCCESS || packet[1] < 7) {
                    printf("[HCI] RSSI complete failed status=0x%02X param_len=%u\n", status, packet[1]);
                }
            }
            break;
        }

        case HCI_EVENT_CONNECTION_COMPLETE: {
            const uint8_t status = hci_event_connection_complete_get_status(packet);
            if (status == 0) {
                const hci_con_handle_t handle = hci_event_connection_complete_get_connection_handle(packet);
                acl_handle = handle;
                bt_rssi = 0;
                hci_event_connection_complete_get_bd_addr(packet, current_device_addr);
                printf("[HCI] ACL connected handle=0x%04X\n", handle);
                printf("[HCI] Request authentication on handle=0x%04X\n", handle);
                hci_send_cmd(&hci_authentication_requested, handle);
            } else {
                device_found = false;
                new_pair = false;
                printf("[HCI] ACL connect failed status=0x%02X\n", status);
                // gap_inquiry_start(30);
            }
            break;
        }

        case HCI_EVENT_LINK_KEY_REQUEST: {
            bd_addr_t addr;
            hci_event_link_key_request_get_bd_addr(packet, addr);
            link_key_t link_key;
            link_key_type_t link_key_type;
            bool link = gap_get_link_key_for_bd_addr(addr, link_key, &link_key_type);
            printf("[HCI] Link key: ");
            for (int i = 0; i < sizeof(link_key_t); i++) {
                printf("%02X", link_key[i]);
            }
            printf("\n");
            if (link) {
                printf("[HCI] Link key request from %s, reply stored key type=%u\n", bd_addr_to_str(addr),
                       (unsigned int) link_key_type);
                hci_send_cmd(&hci_link_key_request_reply, addr, link_key);
            } else {
                printf("[HCI] Link key request from %s, no key, force re-pair\n", bd_addr_to_str(addr));
                hci_send_cmd(&hci_link_key_request_negative_reply, addr);
            }
            break;
        }

        case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
            bd_addr_t addr;
            hci_event_user_confirmation_request_get_bd_addr(packet, addr);
            printf("[HCI] User confirmation request from %s, accept\n", bd_addr_to_str(addr));
            hci_send_cmd(&hci_user_confirmation_request_reply, addr);
            break;
        }

        case HCI_EVENT_PIN_CODE_REQUEST: {
            bd_addr_t addr;
            hci_event_pin_code_request_get_bd_addr(packet, addr);
            printf("[HCI] Legacy pin request from %s, reply 0000\n", bd_addr_to_str(addr));
            gap_pin_code_response(addr, "0000");
            break;
        }

        case HCI_EVENT_AUTHENTICATION_COMPLETE: {
            const uint8_t status = hci_event_authentication_complete_get_status(packet);
            const hci_con_handle_t handle = hci_event_authentication_complete_get_connection_handle(packet);
            printf("[HCI] Authentication complete handle=0x%04X status=0x%02X\n", handle, status);
            if (status != ERROR_CODE_SUCCESS) {
                printf("[HCI] Authentication failed, drop stored key for %s\n", bd_addr_to_str(current_device_addr));
                gap_drop_link_key_for_bd_addr(current_device_addr);
                // gap_inquiry_start(30);
            } else {
                hci_send_cmd(&hci_set_connection_encryption, handle, 1);
            }
            break;
        }

        case HCI_EVENT_ENCRYPTION_CHANGE: {
            const uint8_t status = hci_event_encryption_change_get_status(packet);
            const hci_con_handle_t handle = hci_event_encryption_change_get_connection_handle(packet);
            const uint8_t enabled = hci_event_encryption_change_get_encryption_enabled(packet);
            printf("[HCI] Encryption change handle=0x%04X status=0x%02X enabled=%u\n", handle, status, enabled);
            if (status == ERROR_CODE_SUCCESS && enabled) {
                printf("[L2CAP] Open HID channels\n");
                if (new_pair) {
                    if (hid_control_cid == 0) {
                        l2cap_create_channel(l2cap_packet_handler, current_device_addr, PSM_HID_CONTROL, MTU_CONTROL,
                                             &hid_control_cid);
                    } else if (hid_interrupt_cid == 0) {
                        l2cap_create_channel(l2cap_packet_handler, current_device_addr, PSM_HID_INTERRUPT,
                                             MTU_INTERRUPT,
                                             &hid_interrupt_cid);
                    }
                }
            }
            break;
        }

        case HCI_EVENT_CONNECTION_REQUEST: {
            bd_addr_t addr;
            hci_event_connection_request_get_bd_addr(packet, addr);
            const uint32_t cod = hci_event_connection_request_get_class_of_device(packet);
            printf("[HCI] Incoming ACL request from %s cod=0x%06x\n", bd_addr_to_str(addr), (unsigned int) cod);
            if ((cod & 0x000F00) == 0x000500) {
                bd_addr_copy(current_device_addr, addr);
                gap_inquiry_stop();
                hci_send_cmd(&hci_accept_connection_request, addr, 0x01);
            }
            break;
        }

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
#if !ENABLE_SERIAL && !defined(ENABLE_WAKE_HID)
            // Without ENABLE_WAKE_HID we hide the USB device whenever no
            // controller is paired (upstream behavior). With wake enabled
            // we must stay on the bus across controller power-cycles, so
            // tud_suspend_cb can later fire and tud_remote_wakeup() can
            // signal a wake when the controller is turned back on.
            tud_disconnect();
#endif
            gap_connectable_control(1);
            gap_discoverable_control(1);
            const uint8_t reason = hci_event_disconnection_complete_get_reason(packet);
            device_found = false;
            new_pair = false;
            acl_handle = HCI_CON_HANDLE_INVALID;
            bt_rssi = 0;
            hid_control_cid = 0;
            hid_interrupt_cid = 0;
            feature_data.clear();
            while (queue_try_remove(&send_fifo, NULL)) {}
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
#if ENABLE_BATT_LED
            battery_led_on_disconnect();
#endif
            printf("[HCI] Disconnected reason=0x%02X\n", reason);
            gap_inquiry_start(30);
            bt_inquiring = true;
            break;
        }

        case GAP_EVENT_RSSI_MEASUREMENT: {
            const hci_con_handle_t handle = gap_event_rssi_measurement_get_con_handle(packet);
            if (handle == acl_handle) {
                bt_rssi = static_cast<int8_t>(gap_event_rssi_measurement_get_rssi(packet));
            }
            break;
        }
    }
}

static void l2cap_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void) channel;

    if (packet_type == L2CAP_DATA_PACKET) {
        if (channel == hid_interrupt_cid) {
            // printf("[L2CAP] HID Interrupt data len=%u\n", size);
            // printf_hexdump(packet, size);
            bt_data_callback(INTERRUPT, packet, size);

            // 静默检测
            if (get_config().disable_inactive_disconnect) {
                return;
            }
            if (packet[3] < 120 || packet[3] > 140 ||
                packet[4] < 120 || packet[4] > 140 ||
                packet[5] < 120 || packet[5] > 140 ||
                packet[6] < 120 || packet[6] > 140 ||
                packet[7] > 0 || packet[8] > 0 ||
                packet[10] != 0x08 || packet[11] != 0x00 ||
                packet[12] != 0x00) {
                inactive_time = get_absolute_time();
            } else if (absolute_time_diff_us(inactive_time, get_absolute_time()) >
                       static_cast<int64_t>(get_config().inactive_time) * 60 * 1000 * 1000) {
                printf("disconnect when inactive\n");
                inactive_time = get_absolute_time();
                bt_disconnect();
            }
        } else if (channel == hid_control_cid) {
            if (check_dse) {
                if (packet[0] == 0xA3 && packet[1] == 0x70) {
                    printf("Connected DSE Controller\n");
                    check_dse = false;
                    is_dse = true;
                    // --- DSE profile unlock sequence (mirrors native BT host) ---
                    // 1) SET 0x65: echo of the 0x20 firmware report body, verbatim,
                    //    no CRC recompute (native sends 63 bytes total).
                    if (feature_data.contains(0x20) && feature_data[0x20].size() >= 62) {
                        auto &fw = feature_data[0x20];
                        uint8_t handshake[63];
                        handshake[0] = 0x53;
                        handshake[1] = 0x65;
                        memcpy(handshake + 2, fw.data() + 1, 61);
                        l2cap_send(hid_control_cid, handshake, sizeof(handshake));
                    }
                    // 2) SET 0x80 {0x70,0x01,...}: profile unlock. Must carry a valid
                    //    CRC32 trailer or the controller rejects it with HANDSHAKE
                    //    ERR_INVALID_PARAMETER (0x04). set_feature_data() appends it.
                    uint8_t unlock[59]{};
                    unlock[0] = 0x70;
                    unlock[1] = 0x01;
                    set_feature_data(0x80, unlock, sizeof(unlock));
                    // 3) Connect USB immediately (no delay). The controller needs
                    //    ~3.5s to prepare the profile snapshot; rather than holding
                    //    back enumeration, we mark profiles "busy" until the unlock
                    //    completes. The PS app reads profiles repeatedly during its
                    //    load sequence (capture shows 0x71/0x72 read 24x), so its own
                    //    retry loop converges on real data once dse_profiles_ready.
                    dse_unlock_started_ms = to_ms_since_boot(get_absolute_time());
                    dse_unlock_phase = 1;
                    dse_profiles_ready = false;
#if !ENABLE_SERIAL
                    tud_connect();
#endif
                } else if (packet[0] == 0x02) {
                    printf("Connected DS5 Controller\n");
                    check_dse = false;
                    is_dse = false;
#if !ENABLE_SERIAL
                    tud_connect();
#endif
                }
            }
            if (packet[0] == 0xA3) {
                uint8_t report_id = packet[1];
                feature_data[report_id].assign(packet + 1, packet + size);
#if ENABLE_VERBOSE
                printf("[L2CAP] Stored Feature Report 0x%02X, len=%u\n", report_id, size - 1);
#endif
            }
            // 1-byte HID HANDSHAKE from controller in response to SET commands:
            // 0x00 = SUCCESS, 0x04 = ERR_INVALID_PARAMETER (command rejected)
            if (size == 1 && (packet[0] & 0xF0) == 0x00) {
                printf("[L2CAP] HID HANDSHAKE: 0x%02X (%s)\n", packet[0],
                       packet[0] == 0x00 ? "success" : "rejected");
            }
#if ENABLE_VERBOSE
            printf("[L2CAP] HID Control data len=%u\n", size);
            printf_hexdump(packet, size);
#endif
            bt_data_callback(CONTROL, packet, size);
        } else {
            printf("[L2CAP] Data on unknown channel 0x%04X (Interrupt: 0x%04X, Control: 0x%04X)\n",
                   channel, hid_interrupt_cid, hid_control_cid);
        }
        return;
    }

    const uint8_t event_type = hci_event_packet_get_type(packet);
    switch (event_type) {
        case L2CAP_EVENT_CHANNEL_OPENED: {
            const uint8_t status = l2cap_event_channel_opened_get_status(packet);
            const uint16_t local_cid = l2cap_event_channel_opened_get_local_cid(packet);
            if (status == 0) {
                const uint16_t psm = l2cap_event_channel_opened_get_psm(packet);
                if (psm == PSM_HID_CONTROL) {
                    printf("[L2CAP] HID Control opened cid=0x%04X\n", local_cid);
                    hid_control_cid = local_cid;

                    const auto mtu = l2cap_get_remote_mtu_for_local_cid(hid_control_cid);
                    printf("[L2CAP] Remote Control MTU: %d\n",mtu);
                } else if (psm == PSM_HID_INTERRUPT) {
                    printf("[L2CAP] HID Interrupt opened cid=0x%04X\n", local_cid);
                    hid_interrupt_cid = local_cid;

                    if (!get_config().disable_pico_led) {
                        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
                    }
                    inactive_time = get_absolute_time();

                    printf("Init DualSense\n");

                    init_feature();
                    // 初始化手柄状态
                    state_init();
                    uint8_t report32[142]{};
                    report32[0] = 0x32;
                    report32[1] = 0x10; // reportSeqCounter
                    report32[2] = 0x10 | 0 << 6 | 1 << 7;
                    report32[3] = 0x3f; // 63 bytes
                    state_set(report32 + 4,sizeof(SetStateData));
                    bt_write(report32, sizeof(report32));

                    const auto mtu = l2cap_get_remote_mtu_for_local_cid(hid_interrupt_cid);
                    printf("[L2CAP] Remote Interrupt MTU: %d\n",mtu);

                    wake_on_bt_connect();

                    gap_connectable_control(false);
                    gap_discoverable_control(false);
                    // tud_connect();
                } else {
                    printf("[L2CAP] Unknown Channel psm: 0x%02X", psm);
                }

                /*if (hid_control_cid != 0 && hid_interrupt_cid != 0) {
                    printf("[L2CAP] HID channels ready, request CAN_SEND_NOW for SET_PROTOCOL\n");
                    l2cap_request_can_send_now_event(hid_control_cid);
                }*/
            } else {
                const uint16_t psm = l2cap_event_channel_opened_get_psm(packet);
                hid_control_cid = 0;
                hid_interrupt_cid = 0;
                device_found = false;
                printf("[L2CAP] Open failed psm=0x%04X status=0x%02X\n", psm, status);
                bt_disconnect();
            }
            break;
        }

        case L2CAP_EVENT_INCOMING_CONNECTION: {
            const uint16_t local_cid = l2cap_event_incoming_connection_get_local_cid(packet);
            const uint16_t psm = l2cap_event_incoming_connection_get_psm(packet);
            printf("[L2CAP] Incoming connection psm=0x%04X cid=0x%04X\n", psm, local_cid);
            l2cap_accept_connection(local_cid);
            break;
        }

        case L2CAP_EVENT_CHANNEL_CLOSED: {
            const uint16_t local_cid = l2cap_event_channel_closed_get_local_cid(packet);
            if (local_cid == hid_control_cid) {
                hid_control_cid = 0;
                printf("[L2CAP] HID Control closed cid=0x%04X\n", local_cid);
            } else if (local_cid == hid_interrupt_cid) {
                hid_interrupt_cid = 0;
                printf("[L2CAP] HID Interrupt closed cid=0x%04X\n", local_cid);
            } else {
                printf("[L2CAP] Channel closed cid=0x%04X\n", local_cid);
            }
            if (hid_control_cid == 0 && hid_interrupt_cid == 0) {
                bt_disconnect();
            }
            break;
        }

        case L2CAP_EVENT_CAN_SEND_NOW: {
            // printf("[L2CAP] L2CAP_EVENT_CAN_SEND_NOW\n");

            send_element send_packet{};
            if (queue_try_remove(&send_fifo, &send_packet)) {
                const uint8_t status = l2cap_send(hid_interrupt_cid, send_packet.data, send_packet.len);
                if (status != 0) {
                    printf("[L2CAP] L2CAP Send Error, Status: 0x%02X\n", status);
                }
            }
            if (!queue_is_empty(&send_fifo)) {
                l2cap_request_can_send_now_event(hid_interrupt_cid);
            }
            break;
        }
    }
}

void bt_write(const uint8_t *data, const uint16_t len) {
    if (hid_interrupt_cid == 0) return;
    static send_element packet{};
    memset(packet.data, 0, 512);
    packet.len = len + 1;
    packet.data[0] = 0xA2;
    memcpy(packet.data + 1, data, len);
    fill_output_report_checksum(packet.data + 1, len);

    if (!queue_try_add(&send_fifo, &packet)) {
        printf("[L2CAP bt_write] Error: Failed to add packet to send FIFO\n");
        return;
    }
    if (queue_get_level(&send_fifo) == 1) {
        l2cap_request_can_send_now_event(hid_interrupt_cid);
    }
}

vector<uint8_t> get_feature_data(uint8_t reportId, uint16_t len) {
    // 若为0x81则会请求新内容，其他若有旧数据则不进行请求
    auto ret = vector<uint8_t>{};
    if (feature_data.contains(reportId)) {
        ret = feature_data[reportId];
    }
    if (!feature_data.contains(reportId) ||
        // Get Test Command Result
        reportId == 0x81 ||
        // DSE: Set Profile Save?
        reportId == 0x63 ||
        reportId == 0x65 ||
        reportId == 0x64 ||
        // DSE profile slots: return cache, but refetch in background so the
        // PS app's unlock(0x80) -> re-read flow sees fresh controller data
        (reportId >= 0x70 && reportId <= 0x7B)
    ) {
        if (hid_control_cid != 0) {
            uint8_t get_feature[] = {0x43, reportId};
            l2cap_send(hid_control_cid, get_feature, sizeof(get_feature));
#if ENABLE_VERBOSE
            printf("[L2CAP] Requesting Get Feature Report 0x%02X\n", reportId);
#endif
        }
    }
    return ret;
}

void set_feature_data(uint8_t reportId, uint8_t *data, uint16_t len) {
    if (hid_control_cid != 0) {
        uint8_t get_feature[len + 2];
        get_feature[0] = 0x53;
        get_feature[1] = reportId;
        memcpy(get_feature + 2, data, len);
        fill_feature_report_checksum(get_feature + 1, len + 1);
        l2cap_send(hid_control_cid, get_feature, len + 2);
#if ENABLE_VERBOSE
        printf("[L2CAP] Requesting Set Feature Report 0x%02X\n", reportId);
        printf_hexdump(get_feature, len + 2);
#endif
        // Variant B: single gentle refresh of profile read-back cache after
        // a profile slot write, so the app's verify read sees committed data.
        if (reportId >= 0x60 && reportId <= 0x62) {
            dse_profile_written_ms = to_ms_since_boot(get_absolute_time());
            dse_profile_refresh_round = 0;
        }
    }
}

void bt_power_off_controller() {
    uint8_t bluetooth_control[47]{};
    bluetooth_control[0] = 0x02; // DualSense Bluetooth control: 1=on, 2=off.
    set_feature_data(0x08, bluetooth_control, sizeof(bluetooth_control));
}

// Drives the deferred DSE USB connect. Called from the main loop.
// Phase 1: wait ~4.5s after the 0x80 unlock for the controller to prepare
//          profile data (native capture shows ~3.4s), then prefetch all
//          profile slot reports.
// Phase 2: give the prefetch responses ~700ms to land in the cache, then
//          tud_connect(). The PS app's first GET then returns real data.
void dse_unlock_task() {
    const uint32_t now = to_ms_since_boot(get_absolute_time());

    // Paced prefetch sequencer: one profile GET per 80ms (burst sends drop
    // requests on the L2CAP control channel -> profile shows "not assigned").
    if (dse_prefetch_next != 0 && hid_control_cid != 0) {
        if (now - dse_prefetch_last_ms >= 80) {
            dse_prefetch_last_ms = now;
            get_feature_data(dse_prefetch_next, 64);
            if (dse_prefetch_next == 0x7B) {
                dse_prefetch_next = 0; // done
                if (dse_prefetch_connect) {
                    dse_prefetch_connect = false;
                    dse_profiles_ready = true; // app's retry reads now return real data
                    printf("[DSE] Profile snapshot ready\n");
                }
            } else {
                dse_prefetch_next++;
            }
        }
    } else if (dse_prefetch_next != 0) {
        dse_prefetch_next = 0; // controller gone
        dse_prefetch_connect = false;
    }

    // Post-save snapshot regeneration, mirroring the PS app's own cycle:
    //   SET 0x80  ->  poll GET 0x81 (x6, spaced)  ->  re-read profiles.
    if (dse_profile_written_ms != 0 && hid_control_cid != 0) {
        const uint32_t since_write = now - dse_profile_written_ms;
        if (dse_profile_refresh_round == 0 && since_write >= 500) {
            uint8_t unlock[63]{};
            unlock[0] = 0x70;
            unlock[1] = 0x01;
            set_feature_data(0x80, unlock, sizeof(unlock));
            dse_profile_refresh_round = 1;
            printf("[DSE] Post-save: re-sent 0x80\n");
        } else if (dse_profile_refresh_round >= 1 && dse_profile_refresh_round <= 6 &&
                   since_write >= 1000 + 250u * (dse_profile_refresh_round - 1)) {
            get_feature_data(0x81, 64); // status poll, mirrors app behavior
            dse_profile_refresh_round++;
        } else if (dse_profile_refresh_round == 7 && since_write >= 5500) {
            dse_prefetch_start(false); // paced refetch of 0x70-0x7B
            dse_profile_refresh_round = 8;
            dse_profile_written_ms = 0;
            printf("[DSE] Post-save: profile snapshot refetch started\n");
        }
    }

    if (dse_unlock_phase == 0) {
        return;
    }
    if (hid_control_cid == 0) { // controller went away mid-unlock
        dse_unlock_phase = 0;
        dse_profiles_ready = true; // don't leave profiles gated if it vanished
        return;
    }
    const uint32_t elapsed = now - dse_unlock_started_ms;
    if (dse_unlock_phase == 1 && elapsed >= 4000) {
        // Controller has had ~4s to prepare the snapshot; prefetch it (paced).
        // USB is already connected; completion just flips dse_profiles_ready.
        dse_prefetch_start(true);
        dse_unlock_phase = 0;
        printf("[DSE] Unlock wait done, prefetching profile reports\n");
    }
}

void init_feature() {
    get_feature_data(0x09, 20);
    get_feature_data(0x20, 64);
    get_feature_data(0x22, 64);
    get_feature_data(0x05, 41);
    // DSE
    // check DSE by request 0x70 feature report. DSE return DEFAULT
    // If len == 1, it's DS5
    check_dse = true;
    get_feature_data(0x70, 64);
}
