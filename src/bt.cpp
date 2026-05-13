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
#include "l2cap.h"
#include "pico/cyw43_arch.h"
#include "utils.h"
#include "bsp/board_api.h"
#include "oled.h"
#include "pico/sync.h"
#include "classic/sdp_server.h"
#include "config.h"
#include "pico/util/queue.h"
#include "hci.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/multicore.h"

#define MTU_CONTROL 256
#define MTU_INTERRUPT 1691

// Safe flash offset: 128KB from the end of the flash to avoid BTstack conflict.
#define FLASH_TARGET_OFFSET (PICO_FLASH_SIZE_BYTES - 128 * 1024)

using std::unordered_map;
using std::vector;
using std::queue;

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static void l2cap_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static btstack_packet_callback_registration_t hci_event_callback_registration, l2cap_event_callback_registration;
static bd_addr_t current_device_addr;
static bool device_found = false;
static bool new_pair = false; // 只有新匹配的设备才用创建channel，自动重连走的是service
static hci_con_handle_t acl_handle = HCI_CON_HANDLE_INVALID;

static uint16_t hid_control_cid;
static uint16_t hid_interrupt_cid;
static bt_data_callback_t bt_data_callback = nullptr;
static bool check_dse = false;
unordered_map<uint8_t, vector<uint8_t> > feature_data;

queue_t send_fifo;

struct send_element {
    uint8_t data[512];
    size_t len;
};

absolute_time_t inactive_time = 0; // 手柄长时间静默
static critical_section_t queue_lock;

int current_slot = 0;
static bd_addr_t slot_addrs[4];
static bool slot_occupied[4] = {false, false, false, false};

static bool want_disconnect = false;
static bool want_inquiry = false;
static int pending_slot = -1;

// Magic word to ensure flash isn't garbage
#define FLASH_MAGIC 0x44533501

struct FlashData {
    uint32_t magic;
    bd_addr_t addrs[4];
    bool occupied[4];
};

void save_slots_to_flash() {
    uint8_t buffer[FLASH_PAGE_SIZE] = {0};
    FlashData* data = (FlashData*)buffer;
    data->magic = FLASH_MAGIC;
    memcpy(data->addrs, slot_addrs, sizeof(slot_addrs));
    memcpy(data->occupied, slot_occupied, sizeof(slot_occupied));

    multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_TARGET_OFFSET, buffer, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
    multicore_lockout_end_blocking();
    printf("[Slot] Saved mapping to Flash Memory\n");
}

void load_slots_from_nvm() {
    const FlashData* flash_data = (const FlashData*) (XIP_BASE + FLASH_TARGET_OFFSET);
    
    if (flash_data->magic == FLASH_MAGIC) {
        memcpy(slot_addrs, flash_data->addrs, sizeof(slot_addrs));
        memcpy(slot_occupied, flash_data->occupied, sizeof(slot_occupied));
        
        for (int i = 0; i < 4; i++) {
            if (slot_occupied[i]) {
                printf("[Slot] Loaded Flash MAC %s into Slot %d\n", bd_addr_to_str(slot_addrs[i]), i + 1);
            }
        }
    } else {
        printf("[Slot] Flash empty or invalid magic. Initializing to empty slots.\n");
        for (int i = 0; i < 4; i++) {
            slot_occupied[i] = false;
            memset(slot_addrs[i], 0, 6);
        }
        save_slots_to_flash();
    }
}

bool bt_disconnect() {
    if (acl_handle == HCI_CON_HANDLE_INVALID) {
        return false;
    }

    want_disconnect = true;
    return true;
}

void bt_update() {
    if (want_disconnect && acl_handle != HCI_CON_HANDLE_INVALID) {
        if (hci_can_send_command_packet_now()) {
            gap_disconnect(acl_handle);
            want_disconnect = false;
        }
    } else {
        want_disconnect = false;
    }

    if (want_inquiry && acl_handle == HCI_CON_HANDLE_INVALID) {
        if (hci_can_send_command_packet_now()) {
            if (!slot_occupied[current_slot]) {
                oled_set_status("Scanning...");
                gap_inquiry_start(30);
            } else {
                oled_set_status("Waiting...");
                gap_inquiry_stop(); // Ensure radio is listening, not scanning
            }
            want_inquiry = false;
        }
    }
}

void bt_set_slot(int slot) {
    if (slot < 0 || slot > 3) return;
    if (current_slot == slot) return;
    
    printf("[Slot] Switching to Slot %d\n", slot + 1);
    current_slot = slot;
    
    if (acl_handle != HCI_CON_HANDLE_INVALID) {
        bt_disconnect();
    } else {
        want_inquiry = true;
    }
}

int bt_get_slot() {
    return current_slot;
}

void bt_forget_current_slot() {
    if (slot_occupied[current_slot]) {
        gap_drop_link_key_for_bd_addr(slot_addrs[current_slot]);
        printf("[Slot] Forgot MAC %s for Slot %d\n", bd_addr_to_str(slot_addrs[current_slot]), current_slot + 1);
        
        slot_occupied[current_slot] = false;
        memset(slot_addrs[current_slot], 0, 6);
        save_slots_to_flash();

        if (acl_handle != HCI_CON_HANDLE_INVALID) {
            bt_disconnect();
        } else {
            want_inquiry = true;
        }
    }
}

void bt_clear_all_slots() {
    printf("[Slot] Factory Reset: Clearing ALL slots and pairing data!\n");
    
    btstack_link_key_iterator_t it;
    if (gap_link_key_iterator_init(&it)) {
        bd_addr_t addr;
        link_key_t link_key;
        link_key_type_t type;
        
        bd_addr_t to_delete[10];
        int delete_count = 0;
        
        while (gap_link_key_iterator_get_next(&it, addr, link_key, &type)) {
            if (delete_count < 10) {
                bd_addr_copy(to_delete[delete_count], addr);
                delete_count++;
            }
        }
        gap_link_key_iterator_done(&it);

        for (int i = 0; i < delete_count; i++) {
            gap_drop_link_key_for_bd_addr(to_delete[i]);
            printf("[Slot] Dropped MAC %s\n", bd_addr_to_str(to_delete[i]));
        }
    }
    
    for (int i = 0; i < 4; i++) {
        slot_occupied[i] = false;
        memset(slot_addrs[i], 0, 6);
    }
    save_slots_to_flash();
    
    if (acl_handle != HCI_CON_HANDLE_INVALID) {
        bt_disconnect();
    } else {
        want_inquiry = true;
    }
}

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
    
    // Load slots from flash memory into RAM
    load_slots_from_nvm();
    
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

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void) channel;

    const uint8_t event_type = hci_event_packet_get_type(packet);

    switch (event_type) {
        case BTSTACK_EVENT_STATE: {
            const uint8_t state = btstack_event_state_get_state(packet);
            printf("[BT] State: %u\n", state);
            if (state == HCI_STATE_WORKING) {
                printf("[BT] Stack ready, start inquiry\n");
                oled_set_status("Scanning...");
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
                // Profile Enforcement: Check if this gamepad is allowed in the current slot
                bool allowed = false;
                if (!slot_occupied[current_slot]) {
                    // Current slot is empty. Ensure the gamepad is not owned by another slot.
                    bool owned_by_other = false;
                    for(int i=0; i<4; i++) {
                        if(slot_occupied[i] && memcmp(addr, slot_addrs[i], 6) == 0) {
                            owned_by_other = true; 
                            break;
                        }
                    }
                    if (!owned_by_other) allowed = true;
                } else if (memcmp(addr, slot_addrs[current_slot], 6) == 0) {
                    // Current slot is occupied and the gamepad matches
                    allowed = true;
                }

                if (allowed) {
                    printf("[HCI] Gamepad found and allowed: %s (CoD: 0x%06x)\n", bd_addr_to_str(addr), (unsigned int) cod);
                    bd_addr_copy(current_device_addr, addr);
                    device_found = true;
                    gap_inquiry_stop();
                } else {
                    printf("[HCI] Ignored Gamepad %s (Slot mismatch)\n", bd_addr_to_str(addr));
                }
            }
            break;
        }

        case GAP_EVENT_INQUIRY_COMPLETE:
        case HCI_EVENT_INQUIRY_COMPLETE: {
            printf("[HCI] Inquiry complete.\n");
            if (device_found) {
                printf("[HCI] Connecting to %s...\n", bd_addr_to_str(current_device_addr));
                new_pair = true;
                l2cap_create_channel(l2cap_packet_handler, current_device_addr, PSM_HID_CONTROL, MTU_CONTROL, &hid_control_cid);
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
                printf("[HCI] Create connection rejected, restart inquiry\n");
                want_inquiry = true;
            }
            if (opcode == HCI_OPCODE_HCI_DISCONNECT && status != ERROR_CODE_SUCCESS) {
                printf("[HCI] Disconnect failed (status %02x). Forcing state cleanup.\n", status);
                acl_handle = HCI_CON_HANDLE_INVALID;
                hid_control_cid = 0;
                hid_interrupt_cid = 0;
                want_inquiry = true;
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
                acl_handle = handle;
                hci_event_connection_complete_get_bd_addr(packet, current_device_addr);
                printf("[HCI] ACL connected handle=0x%04X\n", handle);
                
                if (!new_pair) {
                    bool allowed = false;
                    if (slot_occupied[current_slot] && memcmp(current_device_addr, slot_addrs[current_slot], 6) == 0) {
                        allowed = true;
                    }
                    if (!allowed) {
                        printf("[HCI] Reconnection rejected: Slot mismatch\n");
                        bt_disconnect();
                    }
                }
            } else {
                device_found = false;
                new_pair = false;
                printf("[HCI] ACL connect failed status=0x%02X, restart inquiry\n", status);
                // gap_inquiry_start(30);
            }
            break;
        }

        case HCI_EVENT_LINK_KEY_REQUEST: {
            bd_addr_t addr;
            hci_event_link_key_request_get_bd_addr(packet, addr);
            
            // Profile Enforcement at Link Key level:
            bool allowed = false;
            if (!slot_occupied[current_slot]) {
                if (new_pair) {
                    bool owned_by_other = false;
                    for(int i=0; i<4; i++) {
                        if(slot_occupied[i] && memcmp(addr, slot_addrs[i], 6) == 0) {
                            owned_by_other = true; 
                            break;
                        }
                    }
                    if (!owned_by_other) allowed = true;
                }
            } else if (memcmp(addr, slot_addrs[current_slot], 6) == 0) {
                allowed = true;
            }
            
            if (!allowed) {
                printf("[Slot] REJECTED Key Request: %s does not belong here!\n", bd_addr_to_str(addr));
                hci_send_cmd(&hci_link_key_request_negative_reply, addr);
                break;
            }

            link_key_t link_key;
            link_key_type_t link_key_type;
            bool link = gap_get_link_key_for_bd_addr(addr, link_key, &link_key_type);
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
            
            bool allowed = false;
            if (!slot_occupied[current_slot]) {
                if (new_pair) {
                    bool owned_by_other = false;
                    for(int i=0; i<4; i++) {
                        if(slot_occupied[i] && memcmp(addr, slot_addrs[i], 6) == 0) {
                            owned_by_other = true; 
                            break;
                        }
                    }
                    if (!owned_by_other) allowed = true;
                }
            } else if (memcmp(addr, slot_addrs[current_slot], 6) == 0) {
                allowed = true;
            }
            
            if (allowed) {
                printf("[HCI] User confirmation request from %s, accept\n", bd_addr_to_str(addr));
                hci_send_cmd(&hci_user_confirmation_request_reply, addr);
            } else {
                printf("[HCI] REJECTED user confirmation from %s (Ghost Re-Pair Blocked!)\n", bd_addr_to_str(addr));
                hci_send_cmd(&hci_user_confirmation_request_negative_reply, addr);
            }
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
                printf("[HCI] Authentication failed, isolating device %s\n", bd_addr_to_str(current_device_addr));
                bt_disconnect();
            }
            break;
        }

        case HCI_EVENT_ENCRYPTION_CHANGE: {
            const uint8_t status = hci_event_encryption_change_get_status(packet);
            const hci_con_handle_t handle = hci_event_encryption_change_get_connection_handle(packet);
            const uint8_t enabled = hci_event_encryption_change_get_encryption_enabled(packet);
            printf("[HCI] Encryption change handle=0x%04X status=0x%02X enabled=%u\n", handle, status, enabled);
            if (status == ERROR_CODE_SUCCESS && enabled) {
                printf("[HCI] Encryption enabled\n");
                oled_set_status("Connected");
            }
            break;
        }

        case HCI_EVENT_CONNECTION_REQUEST: {
            bd_addr_t addr;
            hci_event_connection_request_get_bd_addr(packet, addr);
            const uint32_t cod = hci_event_connection_request_get_class_of_device(packet);
            printf("[HCI] Incoming ACL request from %s cod=0x%06x\n", bd_addr_to_str(addr), (unsigned int) cod);
            
            // BTstack automatically accepts connections. We just track the address.
            if ((cod & 0x000F00) == 0x000500) {
                bd_addr_copy(current_device_addr, addr);
                gap_inquiry_stop();
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
            hid_control_cid = 0;
            hid_interrupt_cid = 0;
            feature_data.clear();
            while (queue_try_remove(&send_fifo, NULL)) {}
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
            printf("[HCI] Disconnected reason=0x%02X\n", reason);

            if (pending_slot != -1) {
                printf("[Slot] Finalizing switch to Slot %d\n", pending_slot + 1);
                current_slot = pending_slot;
                pending_slot = -1;
            }

            oled_set_status("Disconnected");
            want_disconnect = false; // Clear disconnect flag
            want_inquiry = true;     // Safely queue the inquiry start
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
            if (packet[3] < 120 || packet[3] > 140) {
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
                    bd_addr_t addr;
                    l2cap_event_channel_opened_get_address(packet, addr);
                    
                    bool allowed = false;
                    if (!slot_occupied[current_slot]) {
                        bool owned_by_other = false;
                        for(int i=0; i<4; i++) {
                            if(slot_occupied[i] && memcmp(addr, slot_addrs[i], 6) == 0) {
                                owned_by_other = true; 
                                break;
                            }
                        }
                        if (!owned_by_other) allowed = true;
                    } else if (memcmp(addr, slot_addrs[current_slot], 6) == 0) {
                        allowed = true;
                    }

                    if (!allowed) {
                        printf("[Slot] L2CAP REJECTED: Controller belongs to another slot!\n");
                        bt_disconnect();
                        break;
                    }

                    if (!slot_occupied[current_slot]) {
                        printf("[Slot] ASSIGNING: Locking Slot %d to %s\n", current_slot + 1, bd_addr_to_str(addr));
                        bd_addr_copy(slot_addrs[current_slot], addr);
                        slot_occupied[current_slot] = true;
                        save_slots_to_flash();
                    }

                    printf("[L2CAP] HID Control opened cid=0x%04X\n", local_cid);
                    hid_control_cid = local_cid;
                    
                    if (new_pair) {
                        printf("[L2CAP] Initiating HID Interrupt channel\n");
                        l2cap_create_channel(l2cap_packet_handler, addr, PSM_HID_INTERRUPT, MTU_INTERRUPT, &hid_interrupt_cid);
                    }
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
                    uint8_t report32[142];
                    report32[0] = 0x32;
                    report32[1] = 0x10; // reportSeqCounter
                    uint8_t packet_0x10[] =
                    {
                        0x90, // Packet: 0x10
                        0x3f, // 63
                        // SetStateData
                        0xfd, 0xf7, 0x0, 0x0,
                        0x7f, 0x7f, // Headphones, Speaker
                        0xff, 0x9, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                        0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xa,
                        0x7, 0x0, 0x0, 0x2, 0x1,
                        0x00,
                        0xff, 0xd7, 0x00 // RGB LED: R, G, B (Nijika Color!)✨
                    };
                    memcpy(report32 + 2, packet_0x10, sizeof(packet_0x10));
                    bt_write(report32, sizeof(report32));

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
                printf("[L2CAP] Open failed psm=0x%04X status=0x%02X\n", psm, status);
            }
            break;
        }

        case L2CAP_EVENT_INCOMING_CONNECTION: {
            const uint16_t local_cid = l2cap_event_incoming_connection_get_local_cid(packet);
            const uint16_t psm = l2cap_event_incoming_connection_get_psm(packet);
            
            bd_addr_t addr;
            l2cap_event_incoming_connection_get_address(packet, addr);

            bool allowed = false;
            if (slot_occupied[current_slot] && memcmp(addr, slot_addrs[current_slot], 6) == 0) {
                allowed = true;
            } else if (!slot_occupied[current_slot] && new_pair) {
                allowed = true;
            }

            if (allowed) {
                printf("[L2CAP] Incoming connection psm=0x%04X cid=0x%04X ACCEPTED\n", psm, local_cid);
                l2cap_accept_connection(local_cid);
            } else {
                printf("[L2CAP] Incoming connection psm=0x%04X cid=0x%04X REJECTED (Unauthorized Controller!)\n", psm, local_cid);
                l2cap_decline_connection(local_cid);
                bt_disconnect(); // Hard kick them off the ACL link
            }
            break;
        }

        case L2CAP_EVENT_CHANNEL_CLOSED: {
            const uint16_t local_cid = l2cap_event_channel_closed_get_local_cid(packet);
            bool was_hid = false;
            if (local_cid == hid_control_cid) {
                hid_control_cid = 0;
                printf("[L2CAP] HID Control closed cid=0x%04X\n", local_cid);
                was_hid = true;
            } else if (local_cid == hid_interrupt_cid) {
                hid_interrupt_cid = 0;
                printf("[L2CAP] HID Interrupt closed cid=0x%04X\n", local_cid);
                was_hid = true;
            } else {
                printf("[L2CAP] Other Channel closed cid=0x%04X (SDP?)\n", local_cid);
            }
            if (was_hid && hid_control_cid == 0 && hid_interrupt_cid == 0) {
                bt_disconnect();
            }
            break;
        }

        case L2CAP_EVENT_CAN_SEND_NOW: {
            // printf("[L2CAP] L2CAP_EVENT_CAN_SEND_NOW\n");

            static send_element send_packet{};
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

void bt_write(uint8_t *data, uint16_t len) {
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
        reportId == 0x64
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
