//
// Created by awalol on 2026/3/4.
//

#include "port/port.h"
#include "tusb.h"
#include <cstdio>
#include <cstring>
#include "bt.h"
#include <queue>
#include <unordered_map>
#include <vector>
#include "audio.h"
#include "btstack_event.h"
#include "btstack_tlv.h"
#include "gap.h"
#include "l2cap.h"
#include "utils.h"
#include "classic/sdp_server.h"
#include "config.h"
#include "status_gpio.h"
#include "dse.h"
#include "fake_ds5.h"
#include "wake.h"
#if defined(DS5_HCI_DUMP)
#include "hci_dump.h"
#include "hci_dump_embedded_stdout.h"
#endif
#if ENABLE_BATT_LED
#include "battery_led.h"
#endif
#if PICO_RP2350
#endif

#define MTU_CONTROL 672
#define MTU_INTERRUPT 672

using std::unordered_map;
using std::vector;
using std::queue;


static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
extern uint8_t reportSeqCounter; // main.cpp; per-link, reset when a link comes up
#if defined(DS5_FEATURE_TRACE)
void feature_trace_state_sent();
#endif

static void l2cap_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static btstack_packet_callback_registration_t hci_event_callback_registration, l2cap_event_callback_registration;
static bd_addr_t current_device_addr;

#if defined(PORT_PLATFORM_ESP32S31)
// Last colour the host asked for, so the pulse re-applies it instead of
// overriding it. Defaults to the firmware colour for the case where the pulse
// lands before the host has said anything.
static uint8_t host_led_r = 0xff, host_led_g = 0xd7, host_led_b = 0x00;
#endif
static bool device_found = false;
static bool new_pair = false; // 只有新匹配的设备才用创建channel，自动重连走的是service
bool bt_inquiring = false;

// Inquiry scheduling, ESP32-S31 only. Inquiry and page scan share one radio
// and inquiry wins, so a long inquiry starves the page scan a bonded
// controller's reconnect needs. Espressif's blob lets inquiry dominate where
// CYW43439 interleaves fairly, hence the single target. Explicit discovery
// (BOOTSEL click) still runs one long inquiry.
#if defined(PORT_PLATFORM_ESP32S31)
#define BT_INQUIRY_BURSTS 1
#else
#define BT_INQUIRY_BURSTS 0
#endif


#if BT_INQUIRY_BURSTS
// Burst length, not duty cycle, bounds how long a page train can go unheard,
// so interleaving faster beats holding a quiet window at boot. Not cut below
// two units: an inquiry must stay open long enough to collect responses, which
// arrive after a random backoff.
static constexpr uint8_t kInquiryBurstUnits = 2;      // x1.28s = 2.56s of inquiry
static constexpr uint32_t kInquiryGapMs = 1500;       // page-scan-only between bursts
static constexpr uint32_t kInquiryWindowMs = 45000;   // stop looking after this

static uint32_t inquiry_next_at_ms = 0;   // 0 = nothing scheduled
static uint32_t inquiry_window_end_ms = 0;

static bool use_inquiry_bursts = false;

// Set from the moment a connection is accepted or paged until Connection
// Complete resolves it. acl_handle alone is not enough -- it stays invalid for
// the whole of link setup, so the scheduler would start a burst mid-setup and
// compete with the link it just agreed to bring up.
static bool connection_pending = false;

// Whether connection_pending came from a page we accepted rather than one we
// sent. A device that just paged us plainly does not need discovering.
static bool connection_pending_incoming = false;

static void bt_schedule_inquiry(uint32_t delay_ms) {
    inquiry_next_at_ms = port::now_ms() + delay_ms;
    if (inquiry_next_at_ms == 0) inquiry_next_at_ms = 1; // 0 means "not scheduled"
}

// Bursting only earns its keep when a bonded controller might page us. With no
// stored pairing there is nothing to reconnect, and yielding inquiry time makes
// first-time PS+Share pairing slower and flakier.
static bool bt_has_stored_link_key() {
    btstack_link_key_iterator_t it;
    if (!gap_link_key_iterator_init(&it)) return false;
    bd_addr_t addr;
    link_key_t key;
    link_key_type_t type;
    const bool any = gap_link_key_iterator_get_next(&it, addr, key, &type);
    gap_link_key_iterator_done(&it);
    return any;
}
#endif

#if defined(PORT_PLATFORM_ESP32S31)
// One-shot "release the lights" pulse after connect. The DualSense holds its
// LEDs until the host pulses ResetLights (src/utils.h bit 1.3), and the pulse
// is ignored during the connect animation -- hence the delay, since the init
// report goes out ~2ms after the channel opens.
static constexpr uint32_t kReleaseLightsDelayMs = 3000;
static constexpr uint32_t kReleaseLightsPaintMs = 120; // gap between pulse and paint
static uint32_t release_lights_at_ms = 0;
static uint8_t release_lights_stage = 0;
#endif

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
static int8_t bt_rssi = 0;
unordered_map<uint8_t, vector<uint8_t> > feature_data;
port::Queue send_fifo;

// Control-channel sends are queued: l2cap_send() fails with
// BTSTACK_ACL_BUFFERS_FULL when no ACL buffer is free, and init_feature()
// fires four back to back. ESP32-S31 has fewer ACL buffers than CYW43439.
#if defined(PORT_PLATFORM_ESP32S31)
port::Queue control_send_fifo;
#endif

struct send_element {
    uint8_t data[672];
    size_t len;
};

// Set when hci_create_connection is refused because no command credit is
// available, and retried as soon as one comes back. See the INQUIRY_COMPLETE
// handler for why that happens.
static bool pending_connect = false;

// Do not request authentication the instant the ACL comes up. BlueRetro waits
// first, and only for PlayStation controllers; Bluepad32 never sends
// hci_authentication_requested at all.

uint64_t inactive_time = 0; // 手柄长时间静默 (microseconds since boot)

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
    printf("[HCI] Disconnect requested handle=0x%04X reason=0x13\n", acl_handle);
    hci_send_cmd(&hci_disconnect, acl_handle, 0x13);
    return true;
}

bool bt_is_connected() {
    return hid_interrupt_cid != 0;
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

// Queue a control-channel packet, draining via L2CAP_EVENT_CAN_SEND_NOW rather
// than assuming the controller has a buffer free right now.
static void control_send(const uint8_t *data, const uint16_t len) {
    if (hid_control_cid == 0) return;
#if !defined(PORT_PLATFORM_ESP32S31)
    // RP2350 sends directly. Deferring these broke it: the 0x20 reply drives
    // the only caller of tud_connect(), so the gamepad paired and the USB
    // device never appeared.
    l2cap_send(hid_control_cid, const_cast<uint8_t *>(data), len);
    return;
#else
    static send_element packet{};
    if (len > sizeof(packet.data)) {
        printf("[L2CAP] control packet too large: %u\n", (unsigned) len);
        return;
    }
    packet.len = len;
    memcpy(packet.data, data, len);
    if (!control_send_fifo.try_add(&packet)) {
        printf("[L2CAP] Control send FIFO full, dropping packet\n");
        return;
    }
    if (control_send_fifo.level() == 1) {
        l2cap_request_can_send_now_event(hid_control_cid);
    }
#endif
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
    send_fifo.init(sizeof(send_element), 10);
#if defined(PORT_PLATFORM_ESP32S31)
    control_send_fifo.init(sizeof(send_element), 6);
#endif

    bt_l2cap_init();

    // SSP (Secure Simple Pairing)
    gap_ssp_set_enable(true);
    // Stays ON for both targets: disabling it was tried as a reconnect
    // workaround and retested in isolation, with no benefit.
    gap_secure_connections_enable(true);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_YES_NO);
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);

    gap_connectable_control(1);
    // Stays ON: disabling it on ESP32-S31 broke lightbar handover after a
    // power-cycle and did nothing for reconnect.
    gap_discoverable_control(1);

    hci_event_callback_registration.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    hci_power_control(HCI_POWER_ON);
    return 0;
}

/*int main() {
    stdio_init_all();

    /*while (!stdio_usb_connected()) {
        port::delay_ms(100);
    }
    printf("USB Serial connected!\n");#1#

    bt_init();

    while (1) {
        port::delay_ms(10);
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

// Add an address to the blacklist if not already present (de-duped, capped at
// NVM_NUM_LINK_KEYS). The BOOTSEL-hold clear uses this so repeated holds
// accumulate rather than rebuild (see bt_bootsel_hold_action()).
static void bt_blacklist_add_unique(bd_addr_t addr) {
    if (bt_cleared_addrs_count >= NVM_NUM_LINK_KEYS) return;
    for (int i = 0; i < bt_cleared_addrs_count; i++) {
        if (bd_addr_cmp(addr, bt_cleared_addrs[i]) == 0) return; // already listed
    }
    bd_addr_copy(bt_cleared_addrs[bt_cleared_addrs_count++], addr);
    printf("[BLACKLIST] Added %s\n", bd_addr_to_str(addr));
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
            bt_blacklist_dirty_ms = port::now_ms();
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
    uint32_t now = port::now_ms();
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

    // Additive + de-duped: merge the currently-stored controllers into the
    // EXISTING blacklist. Do NOT reset the list first -- on a second hold no link
    // keys remain, so a rebuild would produce an empty list, and
    // bt_blacklist_persist() delete_tag's an empty list, silently un-blacklisting
    // the controller that was just cleared.
    btstack_link_key_iterator_t it;
    if (gap_link_key_iterator_init(&it)) {
        bd_addr_t addr;
        link_key_t key;
        link_key_type_t type;
        while (gap_link_key_iterator_get_next(&it, addr, key, &type)) {
            bt_blacklist_add_unique(addr);
        }
        gap_link_key_iterator_done(&it);
    }

    // Belt + suspenders: if connected, blacklist the live controller's MAC too
    // (its key may not be persisted yet), then drop the link.
    if (hid_interrupt_cid != 0) {
        bt_blacklist_add_unique(current_device_addr);
        bt_disconnect();
    }
    gap_delete_all_link_keys();
    bt_blacklist_persist();
    printf("[BT] All link keys deleted; %d MAC(s) blacklisted persistently\n",
           bt_cleared_addrs_count);

    bt_clear_flash_toggles_remaining = 12;
    bt_clear_flash_last_toggle_ms = port::now_ms();
}

void bt_note_host_led(uint8_t r, uint8_t g, uint8_t b) {
#if defined(PORT_PLATFORM_ESP32S31)
    host_led_r = r;
    host_led_g = g;
    host_led_b = b;
#else
    // RP2350 hands the lightbar over unprompted, so nothing needs the colour
    // remembered. A no-op here keeps the call site in main.cpp unguarded.
    (void) r; (void) g; (void) b;
#endif
}

void bt_inquiring_led() {
    // BOOTSEL clear-confirmation triple-flash takes priority over inquiry blink
    if (bt_clear_flash_toggles_remaining > 0) {
        uint32_t now = port::now_ms();
        if (now - bt_clear_flash_last_toggle_ms >= 100) {
            bt_clear_flash_last_toggle_ms = now;
            bt_clear_flash_toggles_remaining--;
            port::led_set((bt_clear_flash_toggles_remaining % 2) == 1);
        }
        return;
    }

    if (hid_interrupt_cid != 0) {
        return;
    }
    static bool led_status = false;
    if (!bt_inquiring) {
        if (led_status) {
            port::led_set(false);
        }
        return;
    }
    static auto last_time = port::now_us32();
    if (port::now_us32() - last_time > 200 * 1000) {
        last_time = port::now_us32();
        led_status = !led_status;
        port::led_set(led_status);
    }
}

// hci_send_cmd() does NOT queue: if hci_can_send_command_packet_now() is false
// it logs and returns ERROR_CODE_COMMAND_DISALLOWED, dropping the command
// entirely. Callers therefore have to handle refusal themselves.
static bool bt_try_create_connection() {
    if (!hci_can_send_command_packet_now()) return false;
#if BT_INQUIRY_BURSTS
    connection_pending = true;
    connection_pending_incoming = false;
    inquiry_next_at_ms = 0;
#endif
    const uint8_t rc = hci_send_cmd(&hci_create_connection, current_device_addr,
                                    hci_usable_acl_packet_types(), 0, 0, 0, 1);
    return rc == ERROR_CODE_SUCCESS;
}

// Retry work that could not be issued from an event handler. Called from the
// main loop, where BTstack has finished processing whatever it was doing and
// the command credit is actually available again -- retrying from inside the
// CommandComplete handler is too early, as the credit is restored after app
// callbacks have run.
void bt_task() {
#if defined(PORT_PLATFORM_ESP32S31)
    // Two steps, because one report cannot do both jobs. Carrying ResetLights
    // and a colour together released the LEDs and left them dark: the controller
    // takes the handover and does not apply a colour from the same report, which
    // is why the lightbar blanked until the host next sent something. SDL pulses
    // the bit alone, then paints, so do that.
    if (release_lights_at_ms != 0 && port::now_ms() >= release_lights_at_ms) {
        if (hid_interrupt_cid == 0) {
            release_lights_at_ms = 0;
            release_lights_stage = 0;
        } else if (release_lights_stage == 0) {
            // Step 1: hand the LEDs over. No colour -- AllowLedColor stays 0.
            SetStateData state = {
                .AllowAudioControl = 1,
                .ResetLights = 1,
                .MicSelect = get_config().mic_select,
            };
            update_state(state);
            release_lights_stage = 1;
            release_lights_at_ms = port::now_ms() + kReleaseLightsPaintMs;
            if (release_lights_at_ms == 0) release_lights_at_ms = 1;
        } else {
            // Step 2: paint whatever the host last asked for. In the paths that
            // already worked this repaints the value already showing, so it is
            // invisible there.
            SetStateData state = {
                .AllowAudioControl = 1,
                .AllowLedColor = 1,
                .MicSelect = get_config().mic_select,
                .AllowLightBrightnessChange = 1,
                .AllowColorLightFadeAnimation = 1,
                .LightFadeAnimation = LightFadeAnimation::FadeOut,
                .LightBrightness = LightBrightness::Bright,
                .LedRed = host_led_r,
                .LedGreen = host_led_g,
                .LedBlue = host_led_b,
            };
            update_state(state);
            printf("[%lu] [LED] lights released, painted %02X%02X%02X\n",
                   (unsigned long) port::now_ms(), host_led_r, host_led_g, host_led_b);
            release_lights_at_ms = 0;
            release_lights_stage = 0;
        }
    }
#endif
#if BT_INQUIRY_BURSTS
    if (inquiry_next_at_ms != 0 && port::now_ms() >= inquiry_next_at_ms) {
        inquiry_next_at_ms = 0;
        // Never inquire over a live link -- that is what made the report rate
        // collapse on reconnect, and it is why the connection-request handler
        // calls gap_inquiry_stop().
        if (acl_handle == HCI_CON_HANDLE_INVALID && !connection_pending && !bt_inquiring) {
            // gap_inquiry_start() returns ERROR_CODE_COMMAND_DISALLOWED when
            // no command credit is available; a burst schedule that loses one
            // start would stop looking entirely.
            if (gap_inquiry_start(kInquiryBurstUnits) == ERROR_CODE_SUCCESS) {
                bt_inquiring = true;
            } else {
                bt_schedule_inquiry(250);
            }
        }
    }
#endif

    if (!pending_connect) return;
    if (bt_try_create_connection()) {
        printf("[HCI] Deferred connect sent\n");
        pending_connect = false;
    }
}


static void PORT_FAST_FUNC(hci_packet_handler)(uint8_t packet_type, uint16_t channel, uint8_t *packet,
                                                    uint16_t size) {
    (void) channel;

    const uint8_t event_type = hci_event_packet_get_type(packet);

    switch (event_type) {
        case BTSTACK_EVENT_STATE: {
            const uint8_t state = btstack_event_state_get_state(packet);
            printf("[BT] State: %u\n", state);
            if (state == HCI_STATE_WORKING) {
#if defined(PORT_PLATFORM_ESP32S31)
                // interval 50ms, window 11.25ms. Core Spec 5.4 Vol 2 Part B
                // 8.3.1 requires interval >= 2x window for interlaced scan.
                // Neither HCI command validates the pair, so equal values
                // return success and misconfigure the controller -- and leave
                // no slots for a forming link's LMP exchanges.
                gap_set_page_scan_activity(0x0050, 0x0012);
#else
                gap_set_page_scan_activity(0x0012, 0x0012); // 11.25ms
#endif
                gap_set_page_scan_type(PAGE_SCAN_MODE_INTERLACED);
#if BT_INQUIRY_BURSTS
                bt_blacklist_load();
                use_inquiry_bursts = bt_has_stored_link_key();
                if (use_inquiry_bursts) {
                    printf("[BT] Stack ready, interleaving inquiry with page scan\n");
                    inquiry_window_end_ms = port::now_ms() + kInquiryWindowMs;
                    bt_schedule_inquiry(0); // first burst immediately; gaps follow
                } else {
                    printf("[BT] Stack ready, no pairing stored, start inquiry\n");
                    gap_inquiry_start(30);
                    bt_inquiring = true;
                }
#else
                printf("[BT] Stack ready, start inquiry\n");
                bt_blacklist_load();
                gap_inquiry_start(30);
                bt_inquiring = true;
#endif
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
                // gap_inquiry_stop() above sent HCI_INQUIRY_CANCEL, and BTstack
                // raises GAP_EVENT_INQUIRY_COMPLETE while that command is still
                // outstanding -- so there is often no command credit left here
                // and the connect would be silently discarded. Retry when a
                // credit returns.
                if (!bt_try_create_connection()) {
                    printf("[HCI] Connect deferred (no command credit)\n");
                    pending_connect = true;
                }
                break;
            }
            if (event_type == HCI_EVENT_INQUIRY_COMPLETE) {
                gap_connectable_control(1);
                gap_discoverable_control(1);
#if BT_INQUIRY_BURSTS
                // Another burst after a quiet gap, until the window expires.
                // Upstream stops inquiring entirely here; keeping it going in
                // bursts costs nothing now that page scan gets the gaps.
                if (use_inquiry_bursts && acl_handle == HCI_CON_HANDLE_INVALID &&
                    port::now_ms() < inquiry_window_end_ms) {
                    bt_schedule_inquiry(kInquiryGapMs);
                }
#endif
            }
            break;
        }
        case HCI_EVENT_COMMAND_STATUS: {
            const uint8_t status = hci_event_command_status_get_status(packet);
            const uint16_t opcode = hci_event_command_status_get_command_opcode(packet);
            printf("[%lu] [HCI] CmdStatus %s(0x%04X) status=0x%02X\n",
                   (unsigned long) port::now_ms(), opcode_to_str(opcode), opcode, status);
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
            // Whatever the outcome, the attempt is over -- do not let a stale
            // deferred connect fire again on the next command credit.
            pending_connect = false;
#if BT_INQUIRY_BURSTS
            connection_pending = false;
#endif
            const uint8_t status = hci_event_connection_complete_get_status(packet);
            if (status == 0) {
                const hci_con_handle_t handle = hci_event_connection_complete_get_connection_handle(packet);
                bd_addr_t conn_addr;
                hci_event_connection_complete_get_bd_addr(packet, conn_addr);

                // BTstack auto-accepts incoming connections internally, so our
                // CONNECTION_REQUEST-time reject is racing with hci_run() and
                // doesn't reliably block. Catch the connection here, after it
                // has completed but before we set up any state or request auth,
                // and disconnect immediately with auth-failure reason. Only
                // INCOMING connections are blocked; an outgoing one we initiated
                // has new_pair == true and is allowed through so the blacklist
                // entry can be cleared at HID open.
                if (!new_pair && bt_blacklist_contains(conn_addr)) {
                    printf("[HCI] Incoming connection from blacklisted %s on handle=0x%04X - disconnecting\n",
                           bd_addr_to_str(conn_addr), handle);
                    hci_send_cmd(&hci_disconnect, handle, 0x05);
                    break;
                }

                acl_handle = handle;
                bt_rssi = 0;
#if BT_INQUIRY_BURSTS
                inquiry_next_at_ms = 0; // do not inquire over a live link
#endif
                bd_addr_copy(current_device_addr, conn_addr);
                printf("[HCI] ACL connected handle=0x%04X\n", handle);
#if defined(DS5_HCI_DUMP)
                // Diagnostic build only. Enabled here rather than at startup so
                // the trace begins at the interesting part -- SSP and the L2CAP
                // signalling -- instead of burying it under boot and inquiry.
                hci_dump_init(hci_dump_embedded_stdout_get_instance());
#endif
                printf("[HCI] Request authentication on handle=0x%04X\n", handle);
                hci_send_cmd(&hci_authentication_requested, handle);
            } else {
                device_found = false;
                new_pair = false;
                bd_addr_t fail_addr;
                hci_event_connection_complete_get_bd_addr(packet, fail_addr);
                printf("[%lu] [HCI] ACL connect failed status=0x%02X addr=%s\n",
                       (unsigned long) port::now_ms(), status, bd_addr_to_str(fail_addr));
                if (status == 0x0B) {
                    // ACL Connection Already Exists. Seen after the controller
                    // asserted mid-pairing: one side still believes a link is
                    // up. Resetting the controller clears our half; the
                    // DualSense may need its own reset button.
                    printf("[HCI] (link already exists -- reset the controller if this repeats)\n");
                }
                // Go back to inquiry instead of sitting idle. Without this a
                // single failed connect leaves the dongle dead until a power
                // cycle, which is what made this look like a hang.
                device_found = false;
                new_pair = false;
#if BT_INQUIRY_BURSTS
                // The remote paged us and the attempt failed, so it is nearby
                // and still trying. Inquiry cannot find something already
                // knocking, and takes the radio from the page scan that would
                // hear the next knock.
                if (connection_pending_incoming) {
                    connection_pending_incoming = false;
                    printf("[HCI] Incoming attempt failed; page scanning rather than inquiring\n");
                    break;
                }
                // Re-evaluate rather than latching at boot: the first pairing
                // of a session stores a key, and a dongle that booted with an
                // empty store must switch to bursting once something can page
                // it. Latching meant a failed reconnect restarted a 38s
                // continuous inquiry.
                use_inquiry_bursts = bt_has_stored_link_key();
                if (use_inquiry_bursts) {
                    inquiry_window_end_ms = port::now_ms() + kInquiryWindowMs;
                    bt_schedule_inquiry(kInquiryGapMs);
                } else {
                    gap_inquiry_start(30);
                    bt_inquiring = true;
                }
#else
                gap_inquiry_start(30);
                bt_inquiring = true;
#endif
            }
            break;
        }

        case HCI_EVENT_LINK_KEY_REQUEST: {
            bd_addr_t addr;
            hci_event_link_key_request_get_bd_addr(packet, addr);
            link_key_t link_key;
            link_key_type_t link_key_type;
            bool link = gap_get_link_key_for_bd_addr(addr, link_key, &link_key_type);
            if (link) {
                printf("[HCI] Link key: ");
                for (int i = 0; i < sizeof(link_key_t); i++) {
                    printf("%02X", link_key[i]);
                }
                printf("\n");
            }
            if (link) {
                printf("[HCI] Link key request from %s, reply stored key type=%u\n", bd_addr_to_str(addr),
                       (unsigned int) link_key_type);
#if defined(PORT_PLATFORM_ESP32S31)
                // As in the no-key branch below: BTstack answers link key
                // requests itself (hci.c, AUTH_FLAG_HANDLE_LINK_KEY_REQUEST),
                // reading the same TLV database this printf just read, so the
                // key is identical and ours is purely a duplicate. Unreached
                // until a pairing survives, but it is the same hazard.
                break;
#endif
                hci_send_cmd(&hci_link_key_request_reply, addr, link_key);
            } else {
                printf("[HCI] Link key request from %s, no key, force re-pair\n", bd_addr_to_str(addr));
#if defined(PORT_PLATFORM_ESP32S31)
                // Deliberately do NOT reply: BTstack already sends the negative
                // reply (hci.c, hci_link_key_request_negative_reply). Replying
                // again puts two responses on the wire for one request, the
                // second rejected with 0x0C Command Disallowed.
                break;
#endif
                hci_send_cmd(&hci_link_key_request_negative_reply, addr);
            }
            break;
        }

        case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
            bd_addr_t addr;
            hci_event_user_confirmation_request_get_bd_addr(packet, addr);
            printf("[%lu] [HCI] User confirmation request from %s, accept\n",
                   (unsigned long) port::now_ms(), bd_addr_to_str(addr));
#if defined(PORT_PLATFORM_ESP32S31)
            // Same duplicate-response hazard as the link key request above, and
            // the one that crashed the controller (assert in olm_lmp_ssp.c:1820).
            // BTstack auto-accepts user confirmation (hci_stack->ssp_auto_accept)
            // and replies from hci_run(); ours put a second reply on the wire,
            // arriving after the SSP state machine had moved on.
            break;
#endif
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
#if defined(PORT_PLATFORM_ESP32S31)
                // BTstack requests encryption itself from hci_run() once
                // authentication completes (BONDING_SEND_ENCRYPTION_REQUEST), so
                // ours is a duplicate -- observed on the wire 1ms later, answered
                // 0x0C Command Disallowed.
                (void) handle;
#else
                hci_send_cmd(&hci_set_connection_encryption, handle, 1);
#endif
            }
            break;
        }

        // 0x59 is the Bluetooth 5.4 v2 form of this event, which adds a key
        // size field. The ESP32-S31 controller emits ONLY v2; CYW43439 on the
        // Pico emits only v1, so matching just v1 silently skipped this whole
        // branch on ESP32 and we never opened the HID channels. The first three
        // fields are identical in both, so the v1 getters read v2 correctly.
        case HCI_EVENT_ENCRYPTION_CHANGE_V2:
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
                    }
                }
            }
            break;
        }

        case HCI_EVENT_CONNECTION_REQUEST: {
            bd_addr_t addr;
            hci_event_connection_request_get_bd_addr(packet, addr);
            const uint32_t cod = hci_event_connection_request_get_class_of_device(packet);
            // 这个是按 PS 键重连的时候才会触发
            printf("[HCI] Incoming ACL request from %s cod=0x%06x\n", bd_addr_to_str(addr), (unsigned int) cod);
            if (bt_blacklist_contains(addr)) {
                printf("[HCI] Rejecting connection from %s (MAC is on persistent blacklist; re-pair via PS+Share)\n",
                       bd_addr_to_str(addr));
                hci_send_cmd(&hci_reject_connection_request, addr, 0x0F);
                break;
            }
            if ((cod & 0x000F00) == 0x000500) {
                bd_addr_copy(current_device_addr, addr);
                // 这里的 stop 触发条件是：刚开机时，pico 处于 inquiry 模式，然后 DS5 通过 PS 键重连
                // 如果在连接上以后没有停止 inquiry，会导致回报率很低
                gap_inquiry_stop();
#if BT_INQUIRY_BURSTS
                connection_pending = true;
                connection_pending_incoming = true;
                inquiry_next_at_ms = 0; // drop any burst already scheduled
#endif
#if defined(PORT_PLATFORM_ESP32S31)
                // Left to BTstack, which accepts from hci_run() with the same
                // "remain slave" policy. Accepting here put a second
                // Accept_Connection_Request ahead of gap_inquiry_stop()'s
                // deferred Inquiry_Cancel, asking the controller to accept
                // while still inquiring.
#else
                hci_send_cmd(&hci_accept_connection_request, addr, 0x01);
#endif
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
#if defined(PORT_PLATFORM_ESP32S31)
            release_lights_at_ms = 0;
            release_lights_stage = 0;
#endif
            gpio_on_disconnect();
            while (send_fifo.try_remove(NULL)) {
            }
#if defined(PORT_PLATFORM_ESP32S31)
            while (control_send_fifo.try_remove(NULL)) {
            }
#endif
            port::led_set(false);
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

static void PORT_FAST_FUNC(l2cap_packet_handler)(uint8_t packet_type, uint16_t channel, uint8_t *packet,
                                                      uint16_t size) {
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
                inactive_time = port::now_us();
            } else if (static_cast<int64_t>(port::now_us() - inactive_time) >
                       static_cast<int64_t>(get_config().inactive_time) * 60 * 1000 * 1000) {
                printf("disconnect when inactive\n");
                inactive_time = port::now_us();
                bt_disconnect();
            }
        } else if (channel == hid_control_cid) {
            if (packet[0] == 0xA3) {
                const uint8_t report_id = packet[1];
                feature_data[report_id].assign(packet + 2, packet + size);
#if defined(DS5_FEATURE_TRACE)
                printf("[%lu] [FEAT] reply 0x%02X len=%u\n",
                       (unsigned long) port::now_ms(), report_id, size - 2);
#endif
#if ENABLE_VERBOSE
                printf("[L2CAP] Stored Feature Report 0x%02X, len=%u\n", report_id, size - 2);
                printf("[L2CAP] HID Control data len=%u\n", size);
                printf_hexdump(packet, size);
#endif
                if (report_id == 0x20) {
                    if (packet[23] == 0x44) {
                        printf("Connected DSE Controller\n");
                        is_dse = true;
                        dse_on_connect();

                        if (get_config().controller_mode == 0) {
                            feature_data[0x20].assign(report20,report20 + sizeof(report20));
                        }
                    } else {
                        printf("Connected DS5 Controller\n");
                        is_dse = false;
                    }
#if !ENABLE_SERIAL
                    if (!tud_suspended()) tud_connect();
#endif
                }
            }

            dse_on_control_packet(packet, size);
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
                    printf("[L2CAP] Remote Control MTU: %d\n", mtu);

                    if (new_pair) {
                        printf("[L2CAP] Opening interrupt channel\n");
                        const uint8_t rc = l2cap_create_channel(l2cap_packet_handler, current_device_addr,
                                                                PSM_HID_INTERRUPT, MTU_INTERRUPT,
                                                                &hid_interrupt_cid);
                        if (rc != ERROR_CODE_SUCCESS) {
                            printf("[L2CAP] Interrupt channel create failed rc=0x%02X\n", rc);
                        }
                    }
                } else if (psm == PSM_HID_INTERRUPT) {
                    printf("[L2CAP] HID Interrupt opened cid=0x%04X\n", local_cid);
                    hid_interrupt_cid = local_cid;
                    gpio_on_connect();
                    // Successful pair removes this specific MAC from the persistent
                    // blacklist (treated as user-explicit re-pair in PS+Share mode).
                    bt_blacklist_remove(current_device_addr);

                    if (!get_config().disable_pico_led) {
                        port::led_set(true);
                    }
                    inactive_time = port::now_us();

                    printf("Init DualSense\n");

                    // The output report sequence number is per-LINK. As a plain
                    // global it kept counting across links, so a second connection
                    // began mid-sequence and the DualSense rejected it -- which is
                    // why replugging the dongle (a reboot) worked while a PS-hold
                    // power-cycle did not.
#if defined(PORT_PLATFORM_ESP32S31)
                    reportSeqCounter = 0;
#endif

                    init_feature();
                    SetStateData state = {
                        .AllowAudioControl = 1,
                        .AllowLedColor = 1,
                        .MicSelect = get_config().mic_select,
                        .NoiseCancelEnable = 1,
                        .AllowLightBrightnessChange = 1,
                        .AllowColorLightFadeAnimation = 1,
                        .LightFadeAnimation = LightFadeAnimation::FadeOut,
                        .LightBrightness = LightBrightness::Bright,
                        // RGB LED: R, G, B (Nijika Color!)✨
                        .LedRed = 0xff,
                        .LedGreen = 0xd7,
                        .LedBlue = 0x00,
                    };
                    update_state(state);
#if defined(DS5_FEATURE_TRACE)
                    feature_trace_state_sent();
#endif

#if defined(PORT_PLATFORM_ESP32S31)
                    // Steam never pulses ResetLights (verified: reset=0 on every
                    // host report), and in wireless mode the DualSense holds its own
                    // LEDs until that bit is pulsed -- so after a PS-hold power-cycle
                    // the lightbar stays on the controller's connect blue and ignores
                    // every colour the host asks for.
                    release_lights_stage = 0;
                    release_lights_at_ms = port::now_ms() + kReleaseLightsDelayMs;
                    if (release_lights_at_ms == 0) release_lights_at_ms = 1;
#endif

                    // Re-arm controller mic streaming for the new link. The 0x32
                    // mic-status packet is otherwise only sent when the host opens
                    // or closes the USB mic interface, so a controller that
                    // reconnects while the host still holds that interface open
                    // never gets told to stream and the mic stays silent.
                    update_mic_status();

                    const auto mtu = l2cap_get_remote_mtu_for_local_cid(hid_interrupt_cid);
                    printf("[L2CAP] Remote Interrupt MTU: %d\n", mtu);

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
                printf("[%lu] [L2CAP] HID Control closed cid=0x%04X\n",
                       (unsigned long) port::now_ms(), local_cid);
            } else if (local_cid == hid_interrupt_cid) {
                hid_interrupt_cid = 0;
                printf("[%lu] [L2CAP] HID Interrupt closed cid=0x%04X\n",
                       (unsigned long) port::now_ms(), local_cid);
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

#if defined(PORT_PLATFORM_ESP32S31)
            // The event names the channel it is for; both channels request it.
            const uint16_t ready_cid = l2cap_event_can_send_now_get_local_cid(packet);
            if (ready_cid == hid_control_cid && hid_control_cid != 0) {
                send_element control_packet{};
                if (control_send_fifo.try_remove(&control_packet)) {
                    const uint8_t status = l2cap_send(hid_control_cid, control_packet.data, control_packet.len);
                    if (status != 0) {
                        printf("[L2CAP] Control send error, status: 0x%02X\n", status);
                    }
                }
                if (!control_send_fifo.is_empty()) {
                    l2cap_request_can_send_now_event(hid_control_cid);
                }
                break;
            }
#endif

            send_element send_packet{};
            if (send_fifo.try_remove(&send_packet)) {
                const uint8_t status = l2cap_send(hid_interrupt_cid, send_packet.data, send_packet.len);
                if (status != 0) {
                    printf("[L2CAP] L2CAP Send Error, Status: 0x%02X\n", status);
                }
            }
            if (!send_fifo.is_empty()) {
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
        control_send(data, len);
    }
}

void PORT_FAST_FUNC(bt_write)(const uint8_t *data, const uint16_t len) {
    if (hid_interrupt_cid == 0) return;
    static send_element packet{};
    packet.len = len + 1;
    packet.data[0] = 0xA2;
    memcpy(packet.data + 1, data, len);
    fill_output_report_checksum(packet.data + 1, len);

    if (!send_fifo.try_add(&packet)) {
        printf("[L2CAP bt_write] Error: Failed to add packet to send FIFO\n");
        return;
    }
    if (send_fifo.level() == 1) {
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
        // reportId == 0x81 ||
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
#if defined(DS5_FEATURE_TRACE)
            printf("[%lu] [FEAT] request 0x%02X\n", (unsigned long) port::now_ms(), reportId);
#endif
            control_send(get_feature, sizeof(get_feature));
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
        control_send(get_feature, len + 2);
#if ENABLE_VERBOSE
        printf("[L2CAP] Requesting Set Feature Report 0x%02X\n", reportId);
        printf_hexdump(get_feature, len + 2);
#endif
        dse_on_profile_write(reportId);
    }
}

#if defined(DS5_FEATURE_TRACE)
void feature_trace_state_sent() {
    printf("[%lu] [FEAT] output report sent (lightbar/state)\n", (unsigned long) port::now_ms());
}
#endif

void init_feature() {
    feature_data.clear();
    get_feature_data(0x09, 20);
    get_feature_data(0x20, 64);
    get_feature_data(0x22, 64);
    get_feature_data(0x05, 41);
}

void update_state(const SetStateData &state) {
    uint8_t pkt[142]{};
    pkt[0] = 0x32;
    pkt[1] = 0x10;
    pkt[2] = 0x90;
    pkt[3] = 0x3f;
    memcpy(pkt + 4, &state, sizeof(SetStateData));
    bt_write(pkt, sizeof(pkt));
}
