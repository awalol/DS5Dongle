//
// Created by awalol on 2026/3/4.
//

#include <cstdio>

#include "bt.h"

#include <unordered_map>
#include <vector>

#include "btstack_event.h"
#include "l2cap.h"
#include "pico/cyw43_arch.h"
#include "pico/stdio.h"
#include "utils.h"

static btstack_packet_callback_registration_t hci_event_callback_registration,l2cap_event_callback_registration;
static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void l2cap_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static bd_addr_t current_device_addr;
static bool device_found = false;
static uint16_t hid_control_cid;
static uint16_t hid_interrupt_cid;
static bt_data_callback_t bt_data_callback = NULL;
std::unordered_map<uint8_t, std::vector<uint8_t>> feature_data;

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

int bt_init() {
    if (cyw43_arch_init()) {
        printf("Failed to initialize CYW43\n");
        return 1;
    }

    l2cap_init();
    gap_connectable_control(1);
    gap_discoverable_control(1);
    l2cap_register_service(l2cap_packet_handler, PSM_HID_CONTROL, 0xffff, LEVEL_2);
    l2cap_register_service(l2cap_packet_handler, PSM_HID_INTERRUPT, 0xffff, LEVEL_2);

    hci_event_callback_registration.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);
    l2cap_event_callback_registration.callback = & l2cap_packet_handler;
    l2cap_add_event_handler(&l2cap_event_callback_registration);

    hci_power_control(HCI_POWER_ON);
    return 0;
}

/*int main() {
    stdio_init_all();

    while (!stdio_usb_connected()) {
        sleep_ms(100);
    }
    printf("USB Serial connected!\n");

    init();

    while (1) {
        sleep_ms(10);
    }
}*/

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void) channel;

    const uint8_t event_type = hci_event_packet_get_type(packet);

    switch (event_type) {
        case BTSTACK_EVENT_STATE: {
            const uint8_t state = btstack_event_state_get_state(packet);
            printf("[BT] State: %u\n",state);
            if (state == HCI_STATE_WORKING) {
                printf("[BT] Stack ready, start inquiry\n");
                gap_inquiry_start(30);
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
                printf("[HCI] Gamepad found: %s (CoD: 0x%06x)\n", bd_addr_to_str(addr), (unsigned int)cod);
                bd_addr_copy(current_device_addr, addr);
                device_found = true;
                gap_inquiry_stop();
            }
            break;
        }

        case GAP_EVENT_INQUIRY_COMPLETE:
        case HCI_EVENT_INQUIRY_COMPLETE: {
            printf("[HCI] Inquiry complete\n");
            if (device_found) {
                printf("[HCI] Connecting to %s...\n", bd_addr_to_str(current_device_addr));
                hci_send_cmd(&hci_create_connection, current_device_addr,
                             hci_usable_acl_packet_types(), 0, 0, 0, 1);
            }
            break;
        }
        case HCI_EVENT_COMMAND_STATUS: {
            const uint8_t status = hci_event_command_status_get_status(packet);
            const uint16_t opcode = hci_event_command_status_get_command_opcode(packet);
            printf("[HCI] CmdStatus %s(0x%04X) status=0x%02X\n", opcode_to_str(opcode), opcode, status);
            if (opcode == HCI_OPCODE_HCI_CREATE_CONNECTION && status != ERROR_CODE_SUCCESS) {
                device_found = false;
                printf("[HCI] Create connection rejected, restart inquiry\n");
                gap_inquiry_start(30);
            }
            break;
        }

        case HCI_EVENT_COMMAND_COMPLETE: {
            const uint8_t status = hci_event_command_complete_get_return_parameters(packet)[0];
            const uint16_t opcode = hci_event_command_complete_get_command_opcode(packet);
            printf("[HCI] CmdComplete %s(0x%04X) status=0x%02X\n", opcode_to_str(opcode), opcode, status);
            break;
        }

        case HCI_EVENT_CONNECTION_COMPLETE: {
            const uint8_t status = hci_event_connection_complete_get_status(packet);
            if (status == 0) {
                const hci_con_handle_t handle = hci_event_connection_complete_get_connection_handle(packet);
                hci_event_connection_complete_get_bd_addr(packet, current_device_addr);
                printf("[HCI] ACL connected handle=0x%04X\n", handle);
                printf("[HCI] Request authentication on handle=0x%04X\n", handle);
                hci_send_cmd(&hci_authentication_requested, handle);
            } else {
                device_found = false;
                printf("[HCI] ACL connect failed status=0x%02X, restart inquiry\n", status);
                gap_inquiry_start(30);
            }
            break;
        }

        case HCI_EVENT_LINK_KEY_REQUEST: {
            bd_addr_t addr;
            hci_event_link_key_request_get_bd_addr(packet, addr);
            link_key_t link_key;
            link_key_type_t link_key_type;
            if (gap_get_link_key_for_bd_addr(addr, link_key, &link_key_type)) {
                printf("[HCI] Link key request from %s, reply stored key type=%u\n", bd_addr_to_str(addr),
                       (unsigned int)link_key_type);
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
            }else {
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
                if (hid_control_cid == 0) {
                    l2cap_create_channel(l2cap_packet_handler, current_device_addr, PSM_HID_CONTROL, 0xffff, &hid_control_cid);
                } else if (hid_interrupt_cid == 0){
                    l2cap_create_channel(l2cap_packet_handler,current_device_addr,PSM_HID_INTERRUPT,0xffff,&hid_interrupt_cid);
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
            const uint8_t reason = hci_event_disconnection_complete_get_reason(packet);
            device_found = false;
            hid_control_cid = 0;
            hid_interrupt_cid = 0;
            feature_data.clear();
            printf("[HCI] Disconnected reason=0x%02X, restart inquiry\n", reason);
            gap_inquiry_start(30);
            break;
        }
    }
}

static void l2cap_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void) channel;

    if (packet_type == L2CAP_DATA_PACKET) {
        if (channel == hid_interrupt_cid) {
            printf("[L2CAP] HID Interrupt data len=%u\n", size);
            printf_hexdump(packet, size);
            bt_data_callback(INTERRUPT, packet, size);
        } else if (channel == hid_control_cid) {
            if (size > 1 && packet[0] == 0xA3) {
                uint8_t report_id = packet[1];
                feature_data[report_id].assign(packet + 1, packet + size);
                printf("[L2CAP] Stored Feature Report %d, len=%u\n", report_id, size - 1);
            }
            printf("[L2CAP] HID Control data len=%u\n", size);
            printf_hexdump(packet, size);
            bt_data_callback(CONTROL, packet, size);
        } else {
            printf("[L2CAP] Data on unknown channel 0x%04X (Interrupt: 0x%04X, Control: 0x%04X)\n", channel, hid_interrupt_cid, hid_control_cid);
        }
        return;
    }

    const uint8_t event_type = hci_event_packet_get_type(packet);
    switch (event_type) {
        case L2CAP_EVENT_CHANNEL_OPENED: {
            const uint8_t status = l2cap_event_channel_opened_get_status(packet);
            if (status == 0) {
                const uint16_t psm = l2cap_event_channel_opened_get_psm(packet);

                if (psm == PSM_HID_CONTROL) {
                    printf("[L2CAP] HID Control opened cid=0x%04X\n", hid_control_cid);
                } else if (psm == PSM_HID_INTERRUPT) {
                    printf("[L2CAP] HID Interrupt opened cid=0x%04X\n", hid_interrupt_cid);

                    printf("Ready\n");
                    uint8_t get_feature[41] = {
                        0x43,
                        0x05
                    };
                    l2cap_send(hid_control_cid, get_feature, 41);

                    uint8_t report32[142];
                    report32[0] = 0x32;
                    report32[1] = 0x10;
                    uint8_t packet_0x10[] =
                    {
                        0x90, // Packet: 0x10
                        0x3f, // 63
                        // SetStateData
                        0xfd, 0xf7, 0x0, 0x0, 0x7f, 0x7f,
                        0xff, 0x9, 0x0, 0xf, 0x0, 0x0, 0x0, 0x0,
                        0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xa,
                        0x7, 0x0, 0x0, 0x2, 0x1,
                        0x00,
                        0xff,0xd7,0x00 // RGB LED: R, G, B
                    };
                    memcpy(report32 + 2,packet_0x10,sizeof(packet_0x10));
                    bt_write(report32,sizeof(report32));
                    init_feature();
                }

                if (hid_control_cid != 0 && hid_interrupt_cid != 0) {
                    printf("[L2CAP] HID channels ready, request CAN_SEND_NOW for SET_PROTOCOL\n");
                    l2cap_request_can_send_now_event(hid_control_cid);
                }
            } else {
                const uint16_t psm = l2cap_event_channel_opened_get_psm(packet);
                hid_control_cid = 0;
                hid_interrupt_cid = 0;
                device_found = false;
                printf("[L2CAP] Open failed psm=0x%04X status=0x%02X\n", psm, status);
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
            break;
        }

        case L2CAP_EVENT_CAN_SEND_NOW: {
            printf("L2CAP_EVENT_CAN_SEND_NOW\n");
            
            break;
        }
    }
}

void bt_write(uint8_t* data,uint16_t len) {
    if (!device_found) return;
    uint8_t buffer[len + 1];
    buffer[0] = 0xA2;
    memcpy(buffer + 1,data,len);
    fill_output_report_checksum(buffer + 1,len);
    l2cap_send(hid_interrupt_cid,buffer,sizeof(buffer));
}

uint8_t* get_feature_data(uint8_t reportId,uint16_t len) {
    if (feature_data.find(reportId) == feature_data.end() || feature_data[reportId].empty()) {
        if (hid_control_cid != 0) {
            uint8_t get_feature[] = {0x43, reportId};
            l2cap_send(hid_control_cid, get_feature, len);
            printf("[L2CAP] Requesting Feature Report 0x%02X\n", reportId);
        }
        return 0;
    }
    return feature_data[reportId].data();
}

void init_feature() {
    get_feature_data(0x09,20);
    get_feature_data(0x20,64);
    get_feature_data(0x05,41);
}