//
// Created by awalol on 2026/3/4.
//

#include <cstdio>
#include <cstring>
#include "bt.h"
#include <unordered_map>
#include <vector>
#include "btstack_event.h"
#include "btstack_tlv.h"
#include "gap.h"
#include "l2cap.h"
#include "pico/cyw43_arch.h"
#include "utils.h"
#include "bsp/board_api.h"
#include "classic/sdp_server.h"
#include "config.h"
#include "state_mgr.h"
#include "dse.h"
#include "wake.h"
#ifdef ENABLE_WOL
#include "wol.h"
#endif
#include "pico/util/queue.h"
#if ENABLE_BATT_LED
#include "battery_led.h"
#endif
#include "hardware/sync.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "pico/flash.h"
#if PICO_RP2350
#include "hardware/regs/sio.h"
#endif

#define MTU_CONTROL 672
#define MTU_INTERRUPT 672

using std::unordered_map;
using std::vector;

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static void l2cap_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static btstack_packet_callback_registration_t hci_event_callback_registration, l2cap_event_callback_registration;
static bd_addr_t current_device_addr;
static bool device_found = false;
static bool new_pair = false; // 只有新匹配的设备才用创建channel，自动重连走的是service
bool bt_inquiring = false;

// LED triple-flash confirmation state for clear-all action
static int bt_clear_flash_toggles_remaining = 0;
static uint32_t bt_clear_flash_last_toggle_ms = 0;
// Persistent blacklist of controllers cleared by BOOTSEL hold. Survives
// power-cycles via BTstack TLV flash storage. Blocked at CONNECTION_REQUEST
// so PS-only auto-reconnect fails; INQUIRY_RESULT path is still allowed so
// the user can intentionally re-pair the controller in PS+Share mode, which
// removes that MAC from the blacklist on successful pair.
#define BT_BLACKLIST_TLV_TAG  ((uint32_t) 0x424C434B) // ASCII 'BLCK'
static bd_addr_t bt_cleared_addrs[NVM_NUM_LINK_KEYS];
static int bt_cleared_addrs_count = 0;
// Deferred-persist state: bt_blacklist_remove() sets bt_blacklist_dirty
// instead of writing flash inline (flash_safe_execute() blocks ~50ms with
// interrupts disabled and races with multicore + CYW43 SPI bus, breaking
// pair-completion audio and HID init). The main loop calls
// bt_blacklist_persist_if_dirty() once the connection is stable.
static bool bt_blacklist_dirty = false;
static uint32_t bt_blacklist_dirty_ms = 0;
static hci_con_handle_t acl_handle = HCI_CON_HANDLE_INVALID;
static uint16_t hid_control_cid;
static uint16_t hid_interrupt_cid;
static bt_data_callback_t bt_data_callback = nullptr;
static bool check_dse = false;
static int8_t bt_rssi = 0;
unordered_map<uint8_t, vector<uint8_t> > feature_data;
queue_t send_fifo;

struct send_element {
    uint8_t data[512];
    size_t len;
};

absolute_time_t inactive_time = 0; // 手柄长时间静默

const uint8_t state_init_data[66] = {
    0xa2, 0x31, 0x01,
    0x7f, 0x7d, 0x7f, 0x7e, 0x00, 0x00, 0xa7,
    0x08, 0x00, 0x00, 0x00, 0x52, 0x43, 0x30, 0x41,
    0x01, 0x00, 0x0e, 0x00, 0xef, 0xff, 0x03, 0x03,
    0x7b, 0x1b, 0x18, 0xf0, 0xcc, 0x9c, 0x60, 0x00,
    0xfc, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x09, 0x09, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xa7, 0xad, 0x60, 0x00, 0x29, 0x18, 0x00,
    0x53, 0x9f, 0x28, 0x35, 0xa5, 0xa8, 0x0c, 0x8b
};

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

    gap_set_page_scan_activity(0x0012, 0x0012); // 11.25ms
    gap_set_page_scan_type(PAGE_SCAN_MODE_INTERLACED);
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

// Persist the current bt_cleared_addrs[] blacklist to BTstack TLV flash.
// Empty list -> delete the tag. Called whenever the list changes.
static void bt_blacklist_persist() {
    const btstack_tlv_t *tlv = NULL;
    void *tlv_ctx = NULL;
    btstack_tlv_get_instance(&tlv, &tlv_ctx);
    if (!tlv) {
        printf("[BLACKLIST] No TLV instance available, not persisting\n");
        return;
    }
    if (bt_cleared_addrs_count == 0) {
        tlv->delete_tag(tlv_ctx, BT_BLACKLIST_TLV_TAG);
        printf("[BLACKLIST] Empty, deleted from flash\n");
    } else {
        const uint32_t bytes = bt_cleared_addrs_count * (uint32_t) sizeof(bd_addr_t);
        int rc = tlv->store_tag(tlv_ctx, BT_BLACKLIST_TLV_TAG,
                                (const uint8_t *) bt_cleared_addrs, bytes);
        printf("[BLACKLIST] Persisted %d entries (%lu bytes) to flash, rc=%d\n",
               bt_cleared_addrs_count, bytes, rc);
    }
}

// Load the blacklist from BTstack TLV flash into bt_cleared_addrs[].
// Called once after BTstack reaches HCI_STATE_WORKING.
static void bt_blacklist_load() {
    const btstack_tlv_t *tlv = NULL;
    void *tlv_ctx = NULL;
    btstack_tlv_get_instance(&tlv, &tlv_ctx);
    if (!tlv) {
        bt_cleared_addrs_count = 0;
        return;
    }
    int len = tlv->get_tag(tlv_ctx, BT_BLACKLIST_TLV_TAG,
                           (uint8_t *) bt_cleared_addrs, sizeof(bt_cleared_addrs));
    if (len > 0 && (len % (int) sizeof(bd_addr_t)) == 0) {
        bt_cleared_addrs_count = len / (int) sizeof(bd_addr_t);
        if (bt_cleared_addrs_count > NVM_NUM_LINK_KEYS) {
            bt_cleared_addrs_count = NVM_NUM_LINK_KEYS;
        }
        printf("[BLACKLIST] Loaded %d entries from flash:\n", bt_cleared_addrs_count);
        for (int i = 0; i < bt_cleared_addrs_count; i++) {
            printf("[BLACKLIST]   %s\n", bd_addr_to_str(bt_cleared_addrs[i]));
        }
    } else {
        bt_cleared_addrs_count = 0;
        printf("[BLACKLIST] No persisted entries\n");
    }
}

// Check whether the given address is currently blacklisted.
static bool bt_blacklist_contains(bd_addr_t addr) {
    for (int i = 0; i < bt_cleared_addrs_count; i++) {
        if (bd_addr_cmp(addr, bt_cleared_addrs[i]) == 0) return true;
    }
    return false;
}

// Remove the given address from the blacklist (if present). Defers the
// flash persist to the main loop via bt_blacklist_dirty so the L2CAP HID
// open hot path stays fast (audio + HID init must not block on flash).
static void bt_blacklist_remove(bd_addr_t addr) {
    for (int i = 0; i < bt_cleared_addrs_count; i++) {
        if (bd_addr_cmp(addr, bt_cleared_addrs[i]) == 0) {
            // Shift remaining entries down
            for (int j = i; j < bt_cleared_addrs_count - 1; j++) {
                bd_addr_copy(bt_cleared_addrs[j], bt_cleared_addrs[j + 1]);
            }
            bt_cleared_addrs_count--;
            printf("[BLACKLIST] Removed %s on successful pair, %d remaining (persist deferred)\n",
                   bd_addr_to_str(addr), bt_cleared_addrs_count);
            bt_blacklist_dirty = true;
            bt_blacklist_dirty_ms = to_ms_since_boot(get_absolute_time());
            return;
        }
    }
}

// Called from the main loop. If the blacklist has been modified in RAM
// (by bt_blacklist_remove()) and a settle window has passed since the last
// modification, persist it to flash. The settle window ensures we never
// take the flash blackout while the controller is still negotiating its
// initial HID/audio state right after pair completion.
void bt_blacklist_persist_if_dirty() {
    if (!bt_blacklist_dirty) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - bt_blacklist_dirty_ms < 5000) return;
    bt_blacklist_dirty = false;
    printf("[BLACKLIST] Settle window elapsed, persisting deferred change\n");
    bt_blacklist_persist();
}

// BOOTSEL click action: trigger a fresh inquiry to pair another controller.
// If currently connected, disconnect (which triggers inquiry restart via the
// disconnect handler) - link key is preserved so the disconnected controller
// can still reconnect later.
void bt_bootsel_click_action() {
    printf("[BT] BOOTSEL click - fresh inquiry\n");
    if (hid_interrupt_cid != 0) {
        bt_disconnect();
    } else {
        gap_inquiry_start(30);
        bt_inquiring = true;
    }
}

// BOOTSEL hold action: disconnect current controller and delete all link keys.
// Snapshots the cleared addresses (from stored keys and the currently-connected
// MAC) into the persistent blacklist so PS-only auto-reconnect is blocked even
// across power-cycles. The MAC is removed from the blacklist when the user
// explicitly re-pairs the controller (in PS+Share mode) and L2CAP HID opens.
// Triggers a six-flash LED confirmation via bt_inquiring_led().
void bt_bootsel_hold_action() {
    printf("[BT] BOOTSEL held - clearing all pairings\n");

    // Reset and rebuild blacklist from currently stored keys
    bt_cleared_addrs_count = 0;
    btstack_link_key_iterator_t it;
    if (gap_link_key_iterator_init(&it)) {
        bd_addr_t addr;
        link_key_t key;
        link_key_type_t type;
        while (gap_link_key_iterator_get_next(&it, addr, key, &type) &&
               bt_cleared_addrs_count < NVM_NUM_LINK_KEYS) {
            bd_addr_copy(bt_cleared_addrs[bt_cleared_addrs_count++], addr);
            printf("[BLACKLIST] From stored key: %s\n", bd_addr_to_str(addr));
        }
        gap_link_key_iterator_done(&it);
    }

    // Belt + suspenders: if connected, add the live controller's MAC too,
    // in case the iterator missed it (e.g. key not yet persisted to flash).
    if (hid_interrupt_cid != 0 && bt_cleared_addrs_count < NVM_NUM_LINK_KEYS) {
        bool already_present = false;
        for (int i = 0; i < bt_cleared_addrs_count; i++) {
            if (bd_addr_cmp(current_device_addr, bt_cleared_addrs[i]) == 0) {
                already_present = true;
                break;
            }
        }
        if (!already_present) {
            bd_addr_copy(bt_cleared_addrs[bt_cleared_addrs_count++], current_device_addr);
            printf("[BLACKLIST] From live connection: %s\n", bd_addr_to_str(current_device_addr));
        }
    }

    if (hid_interrupt_cid != 0) {
        bt_disconnect();
    }
    gap_delete_all_link_keys();
    bt_blacklist_persist();
    printf("[BT] All link keys deleted; %d MAC(s) blacklisted persistently\n",
           bt_cleared_addrs_count);

    bt_clear_flash_toggles_remaining = 12;
    bt_clear_flash_last_toggle_ms = to_ms_since_boot(get_absolute_time());
}

void bt_inquiring_led() {
    // BOOTSEL clear-confirmation triple-flash takes priority over inquiry blink
    if (bt_clear_flash_toggles_remaining > 0) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - bt_clear_flash_last_toggle_ms >= 100) {
            bt_clear_flash_last_toggle_ms = now;
            bt_clear_flash_toggles_remaining--;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, (bt_clear_flash_toggles_remaining % 2) == 1);
        }
        return;
    }

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

static void __not_in_flash_func(hci_packet_handler)(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void) channel;

    const uint8_t event_type = hci_event_packet_get_type(packet);

    switch (event_type) {
        case BTSTACK_EVENT_STATE: {
            const uint8_t state = btstack_event_state_get_state(packet);
            printf("[BT] State: %u\n", state);
            if (state == HCI_STATE_WORKING) {
                printf("[BT] Stack ready, start inquiry\n");
                bt_blacklist_load();
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
            // Blacklisted MACs are NOT filtered here so the user can intentionally
            // re-pair them in PS+Share mode (dongle-initiated path). PS-only
            // (controller-initiated) is blocked at CONNECTION_REQUEST.
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
                bd_addr_t conn_addr;
                hci_event_connection_complete_get_bd_addr(packet, conn_addr);

                // BTstack auto-accepts incoming connections internally, so our
                // CONNECTION_REQUEST-time reject is racing with hci_run() and
                // doesn't reliably block. Catch the connection here, after it
                // has completed but before we set up any state or request auth,
                // and disconnect immediately with auth-failure reason.
                //
                // Only block INCOMING connections (controller PAGE'd us, e.g. PS-only
                // auto-reconnect). Outgoing connections that we initiated via inquiry
                // (PS+Share user-explicit re-pair) have new_pair == true and are
                // allowed through so the blacklist entry can be removed at HID open.
                if (!new_pair && bt_blacklist_contains(conn_addr)) {
                    printf("[HCI] Incoming connection from blacklisted %s on handle=0x%04X - disconnecting\n",
                           bd_addr_to_str(conn_addr), handle);
                    hci_send_cmd(&hci_disconnect, handle, 0x05);
                    break;
                }

                acl_handle = handle;
                bt_rssi = 0;
                bd_addr_copy(current_device_addr, conn_addr);
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
            if (bt_blacklist_contains(addr)) {
                printf("[HCI] Rejecting connection from %s (MAC is on persistent blacklist; re-pair via PS+Share)\n", bd_addr_to_str(addr));
                hci_send_cmd(&hci_reject_connection_request, addr, 0x0F);
                break;
            }
            if ((cod & 0x000F00) == 0x000500) {
                bd_addr_copy(current_device_addr, addr);
                gap_inquiry_stop();
                hci_send_cmd(&hci_accept_connection_request, addr, 0x01);
            }
            break;
        }

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
#if !ENABLE_SERIAL
            // Hide the USB device when no controller is paired (upstream behavior), EXCEPT when
            // wake is on (stay on the bus so a returning controller can signal a host wake) or
            // while the host is suspended -- hiding then re-showing re-enumerates, and a USB
            // re-connect wakes a sleeping host. Defer the hide until the host is awake.
            if (!get_config().enable_wake && !tud_suspended()) {
                tud_disconnect();
            }
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
            while (queue_try_remove(&send_fifo, NULL)) {}
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
#if ENABLE_BATT_LED
            battery_led_on_disconnect();
#endif
            printf("[HCI] Disconnected reason=0x%02X\n", reason);
            bt_data_callback(INTERRUPT, const_cast<uint8_t *>(state_init_data), sizeof(state_init_data));
            // gap_inquiry_start(30);
            // bt_inquiring = true;
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

static void __not_in_flash_func(l2cap_packet_handler)(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void) channel;

    if (packet_type == L2CAP_DATA_PACKET) {
        if (channel == hid_interrupt_cid) {
            // printf("[L2CAP] HID Interrupt data len=%u\n", size);
            // printf_hexdump(packet, size);
            bt_data_callback(INTERRUPT, packet, size);

            // 静默检测
            if (!(packet[2] & 1) || get_config().inactive_time == 0) {
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
                    // Unlock Edge profiles; USB connects immediately, profile
                    // reads are gated until the snapshot is prepared.
                    dse_on_connect();
#if !ENABLE_SERIAL
                    // don't re-enumerate while the host is suspended -- it would wake a sleeping host
                    if (!tud_suspended()) tud_connect();
#endif
                } else if (packet[0] == 0x02) {
                    printf("Connected DS5 Controller\n");
                    check_dse = false;
                    is_dse = false;
#if !ENABLE_SERIAL
                    if (!tud_suspended()) tud_connect();
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
            dse_on_control_packet(packet, size);
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
                    // Successful pair removes this specific MAC from the persistent
                    // blacklist (treated as user-explicit re-pair in PS+Share mode).
                    bt_blacklist_remove(current_device_addr);

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
#ifdef ENABLE_WOL
                    // The controller has just connected: if the PC appears to be
                    // off (does not enumerate over USB), send the Wake-on-LAN.
                    wol_request();
#endif

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

// Accessors used by the DSE profile module (dse.cpp).
uint16_t bt_control_cid() {
    return hid_control_cid;
}

void bt_control_send(const uint8_t *data, uint16_t len) {
    if (hid_control_cid != 0) {
        l2cap_send(hid_control_cid, const_cast<uint8_t *>(data), len);
    }
}

void __not_in_flash_func(bt_write)(const uint8_t *data, const uint16_t len) {
    if (hid_interrupt_cid == 0) return;
    static send_element packet{};
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
    const bool has_cached_report = feature_data.contains(reportId);
    if (has_cached_report) {
        ret = feature_data[reportId];
    }
    if (!has_cached_report ||
        // Get Test Command Result
        reportId == 0x81 ||
        // DSE: Set Profile Save?
        reportId == 0x63 ||
        reportId == 0x65 ||
        reportId == 0x64 ||
        // DSE profile slots: return cache, but refetch in background so the
        // PS app's unlock(0x80) -> re-read flow sees fresh controller data.
        dse_is_profile_report(reportId)
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

bool set_feature_data(uint8_t reportId, uint8_t *data, uint16_t len) {
    if (hid_control_cid == 0) {
        return false;
    }
    uint8_t get_feature[len + 2];
    get_feature[0] = 0x53;
    get_feature[1] = reportId;
    memcpy(get_feature + 2, data, len);
    fill_feature_report_checksum(get_feature + 1, len + 1);
    // l2cap_send does NOT queue: it returns BTSTACK_ACL_BUFFERS_FULL and drops
    // the packet when the single outgoing ACL buffer is busy (e.g. the interrupt
    // channel still draining rumble/audio). Report that to the caller.
    const uint8_t status = l2cap_send(hid_control_cid, get_feature, len + 2);
#if ENABLE_VERBOSE
    printf("[L2CAP] Requesting Set Feature Report 0x%02X (status=%d)\n", reportId, status);
    printf_hexdump(get_feature, len + 2);
#endif
    dse_on_profile_write(reportId);
    return status == ERROR_CODE_SUCCESS;
}

bool bt_power_off_controller() {
    if (hid_control_cid == 0) {
        return true; // no controller connected: nothing to power off
    }
    uint8_t bluetooth_control[47]{};
    bluetooth_control[0] = 0x02; // DualSense Bluetooth control: 1=on, 2=off.
    return set_feature_data(0x08, bluetooth_control, sizeof(bluetooth_control));
}

void init_feature() {
    feature_data.clear();
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
