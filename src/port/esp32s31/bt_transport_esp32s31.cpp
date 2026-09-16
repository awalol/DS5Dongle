// BTstack HCI transport over the ESP32-S31 on-die controller (VHCI).
//
// Uses the pollable btstack_run_loop_embedded, pumped from the superloop, not
// BTstack's port/esp32 whose FreeRTOS run loop never returns. Every BTstack
// callback therefore runs on the main task, which the shared firmware requires:
// bt_write() uses a single static send_element and the report counters are
// non-atomic. The firmware is NOT single-threaded though -- the core-1 audio
// worker and port::timer_once_ms() are elsewhere. See the threading section of
// ports/esp32s31/README.md.
//
// VHCI callbacks come from the controller's own task and touch only a
// mutex-guarded ring buffer, drained by the main task. BTstack's own deferral
// (btstack_run_loop_execute_on_main_thread) is an unlocked list insert on the
// embedded run loop, so the explicit ring is required.

#include "../port.h"

#include <cstdio>
#include <cstring>

#include "esp_bt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// Undocumented, unheadered, but a global symbol in libbredr_app.a and already
// linked into the image (see the .map). Signature from disassembly.
extern "C" int r_orca_log_set_level(int module, int level);

extern "C" {
#include "btstack_memory.h"
#include "btstack_ring_buffer.h"
#include "btstack_run_loop.h"
#include "btstack_run_loop_embedded.h"
#include "btstack_tlv.h"
#include "btstack_tlv_esp32.h"
#include "classic/btstack_link_key_db_tlv.h"
#include "hci.h"
#include "hci_transport.h"
}

namespace {

// Sized like BTstack's own port: enough ACL packets to cover a stall plus a few
// events. SCO is omitted -- the DualSense carries audio over L2CAP, not a voice
// link, and the sdkconfig builds BR/EDR without SCO buffers.
constexpr uint16_t kAclPacketLen = 1021;
constexpr uint16_t kAclPacketNum = 8;
constexpr uint16_t kEventPacketNum = 4;

// Each entry is a 2-byte length tag plus the H4 packet (type byte + payload).
uint8_t g_ring_storage[kAclPacketNum * (2 + 1 + HCI_ACL_HEADER_SIZE + kAclPacketLen) +
                       kEventPacketNum * (2 + 1 + HCI_EVENT_BUFFER_SIZE)];

btstack_ring_buffer_t g_ring;
SemaphoreHandle_t g_ring_mutex = nullptr;

// BTstack wants a pre-buffer ahead of the packet it is handed.
uint8_t g_rx_with_pre_buffer[HCI_INCOMING_PRE_BUFFER_SIZE + HCI_INCOMING_PACKET_BUFFER_SIZE];
uint8_t *const g_rx = &g_rx_with_pre_buffer[HCI_INCOMING_PRE_BUFFER_SIZE];

void (*g_packet_handler)(uint8_t packet_type, uint8_t *packet, uint16_t size) = nullptr;

// Set by the controller (on its own task) when it has taken the packet we
// handed it and can accept another. Cleared by the main task, which then
// raises HCI_EVENT_TRANSPORT_PACKET_SENT so BTstack releases its packet buffer.
volatile bool g_tx_complete = false;

// Incremented by the controller task, drained by the main task for reporting.
volatile uint32_t g_rx_dropped = 0;

bool g_controller_started = false;

// ---------------------------------------------------------- VHCI callbacks
// These run on the "BT Controller" task, not the main task.

void vhci_send_available_cb() {
    // The controller has capacity again. Harmless to note, but not what
    // releases BTstack's buffer -- see transport_send_packet().
    g_tx_complete = true;
}

int vhci_recv_cb(uint8_t *data, uint16_t len) {
    if (g_ring_mutex == nullptr) return 0;

    xSemaphoreTake(g_ring_mutex, portMAX_DELAY);
    if (btstack_ring_buffer_bytes_free(&g_ring) < static_cast<uint32_t>(len) + 2) {
        xSemaphoreGive(g_ring_mutex);
        // Counted, not printed. This runs on the BT controller task, and the
        // console is a blocking UART behind a mutex the main task holds
        // constantly -- stalling the controller here would cause the very
        // disconnects this transport exists to avoid. bt_transport_poll()
        // reports the delta from the main task instead.
        ++g_rx_dropped;
        return 0;
    }

    uint8_t len_tag[2];
    little_endian_store_16(len_tag, 0, len);
    btstack_ring_buffer_write(&g_ring, len_tag, sizeof(len_tag));
    btstack_ring_buffer_write(&g_ring, data, len);
    xSemaphoreGive(g_ring_mutex);
    return 0;
}

const esp_vhci_host_callback_t g_vhci_cb = {
    .notify_host_send_available = vhci_send_available_cb,
    .notify_host_recv = vhci_recv_cb,
};

// ------------------------------------------------------------- hci_transport

void transport_init(const void *) {}

int transport_open() {
    btstack_ring_buffer_init(&g_ring, g_ring_storage, sizeof(g_ring_storage));

    // esp_bt_controller_init() may only be called once per boot.
    if (!g_controller_started) {
        esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        esp_err_t ret = esp_bt_controller_init(&cfg);
        if (ret != ESP_OK) {
            printf("[BT] esp_bt_controller_init failed: %s\n", esp_err_to_name(ret));
            return -1;
        }
        g_controller_started = true;
    }

    // On esp32s31 the mode argument is ignored -- controller/esp32s31/bt.c has
    // UNUSED(mode), and BR/EDR vs LE is fixed at compile time by
    // CONFIG_BTDM_CTRL_MODE_* (we set BR_EDR_ONLY). Passed correctly anyway so
    // this does not become a lie if that changes.
    const esp_err_t ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        printf("[BT] esp_bt_controller_enable failed: %s\n", esp_err_to_name(ret));
        return -1;
    }

    // The closed BR/EDR blob has its own log framework with no Kconfig.
    // r_orca_log_init silences all 15 modules during controller init, so changes
    // must come after enable(). Lower is more verbose ([E]/[W] <=4, [D] <=1).
    // Useful modules: 5 OLC (paging), 6 OLM, 8 OLM_LMP, 9 OLM_CONN. Left silent
    // -- module 5 at level 1 buries the console while page scan runs.
#if defined(DS5_BT_CONTROLLER_LOG)
    r_orca_log_set_level(9, 1);
    r_orca_log_set_level(5, 1);
    r_orca_log_set_level(6, 4);
    r_orca_log_set_level(8, 1);
    r_orca_log_set_level(7, 4);
    printf("[BT] controller internal log enabled (OLC/OLM_CONN debug)\n");
#endif

    esp_vhci_host_register_callback(&g_vhci_cb);
    return 0;
}

int transport_close() {
    esp_bt_controller_disable();
    return 0;
}

void transport_register_packet_handler(void (*handler)(uint8_t, uint8_t *, uint16_t)) {
    g_packet_handler = handler;
}

int transport_can_send_packet_now(uint8_t) {
    return esp_vhci_host_check_send_available();
}

// Bounce buffer for outgoing packets. esp_vhci_host_send_packet() is
// asynchronous but BTstack frees its single HCI packet buffer on
// HCI_EVENT_TRANSPORT_PACKET_SENT, so passing BTstack's buffer lets it refill
// that memory underneath the controller. Waiting on the send-available callback
// instead is not viable: it only fires on a transition back to capacity, so
// after an ordinary send it never arrives.
uint8_t g_tx_buffer[1 + HCI_INCOMING_PACKET_BUFFER_SIZE];

int transport_send_packet(uint8_t packet_type, uint8_t *packet, int size) {
    // VHCI takes H4 framing: one type byte then the payload.
    const int buffer_size = size + 1;
    if (buffer_size > static_cast<int>(sizeof(g_tx_buffer))) {
        printf("[BT] outgoing HCI packet too large: %d\n", buffer_size);
        return -1;
    }
    g_tx_buffer[0] = packet_type;
    memcpy(&g_tx_buffer[1], packet, static_cast<size_t>(size));
    esp_vhci_host_send_packet(g_tx_buffer, buffer_size);

    // Safe to release BTstack's buffer now: the controller is reading our copy,
    // not theirs.
    g_tx_complete = true;
    return 0;
}

const hci_transport_t g_transport = {
    "esp32s31-vhci",
    &transport_init,
    &transport_open,
    &transport_close,
    &transport_register_packet_handler,
    &transport_can_send_packet_now,
    &transport_send_packet,
    nullptr, // set_baudrate
    nullptr, // reset_link
    nullptr, // set_sco_config
};

} // namespace

// BTstack's embedded run loop asks for wall time via hal_time_ms() because
// src/btstack_config.h defines HAVE_EMBEDDED_TIME_MS.
extern "C" uint32_t hal_time_ms(void) {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

// The embedded run loop guards its trigger flag with the embedded HAL's
// irq primitives. A FreeRTOS critical section is the SMP-correct equivalent:
// it masks interrupts on this core and takes a spinlock. The run loop only
// holds it around a flag test, so it stays short enough to be safe.
namespace {
portMUX_TYPE g_hal_mux = portMUX_INITIALIZER_UNLOCKED;
} // namespace

extern "C" void hal_cpu_disable_irqs(void) { portENTER_CRITICAL_SAFE(&g_hal_mux); }
extern "C" void hal_cpu_enable_irqs(void) { portEXIT_CRITICAL_SAFE(&g_hal_mux); }

extern "C" void hal_cpu_enable_irqs_and_sleep(void) {
    // Deliberately does NOT sleep. The embedded run loop calls this when it has
    // no work, but our caller is main.cpp's superloop, which still has to
    // service TinyUSB and pet the watchdog. Sleeping here would stall USB.
    portEXIT_CRITICAL_SAFE(&g_hal_mux);
}

namespace port {

bool bt_transport_init() {
    g_ring_mutex = xSemaphoreCreateMutex();
    if (g_ring_mutex == nullptr) {
        printf("[BT] could not create HCI ring mutex\n");
        return false;
    }

    // Everything cyw43_arch_init() did for the Pico build, since bt.cpp only
    // does sdp_init()/l2cap_init()/hci_power_control() on top.
    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_embedded_get_instance());

    const btstack_tlv_t *tlv = btstack_tlv_esp32_get_instance();
    if (tlv != nullptr) {
        // Link keys and the firmware's own blacklist tags live in NVS.
        btstack_tlv_set_instance(tlv, nullptr);
    } else {
        printf("[BT] NVS-backed TLV unavailable; pairings will not persist\n");
    }

    hci_init(&g_transport, nullptr);

    // AFTER hci_init(), which allocates hci_stack -- hci_set_link_key_db()
    // dereferences it immediately -- and before hci_power_control(). Setting
    // the TLV instance alone is not enough: link keys live behind a separate
    // hci_stack->link_key_db, and left null it answers every
    // HCI_Link_Key_Request negatively, so PS-only reconnect cannot work.
    if (tlv != nullptr) {
        hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(tlv, nullptr));
    }

    return true;
}

// Synthetic "transport done with your buffer" event. BTstack releases its
// single HCI packet buffer only on this (hci.c: HCI_EVENT_TRANSPORT_PACKET_SENT),
// so it must fire between packets during a drain, not once at the end.
static void emit_packet_sent_if_pending() {
    if (!g_tx_complete) return;
    g_tx_complete = false;
    if (g_packet_handler == nullptr) return;
    uint8_t event[] = {HCI_EVENT_TRANSPORT_PACKET_SENT, 0};
    g_packet_handler(HCI_EVENT_PACKET, event, sizeof(event));
}

void bt_transport_poll() {
    // Deliver anything the controller task queued. Copy out under the mutex,
    // then dispatch with it released -- the handler re-enters BTstack and can
    // end up sending, and holding the lock across that would invite deadlock.
    while (true) {
        // Release the packet buffer before dispatching the next event. Without
        // this, a handler that sends (authentication_requested,
        // link_key_request_reply, user_confirmation_reply,
        // set_connection_encryption, disconnect -- i.e. the whole SSP path)
        // is silently refused for the rest of the drain. Only the connect had
        // a retry; none of the others do.
        emit_packet_sent_if_pending();

        uint32_t got = 0;
        uint8_t len_tag[2];

        xSemaphoreTake(g_ring_mutex, portMAX_DELAY);
        if (btstack_ring_buffer_bytes_available(&g_ring) == 0) {
            xSemaphoreGive(g_ring_mutex);
            break;
        }
        btstack_ring_buffer_read(&g_ring, len_tag, sizeof(len_tag), &got);
        const uint32_t len = little_endian_read_16(len_tag, 0);
        btstack_ring_buffer_read(&g_ring, g_rx, len, &got);
        xSemaphoreGive(g_ring_mutex);

        if (g_packet_handler != nullptr && len >= 1) {
            g_packet_handler(g_rx[0], &g_rx[1], static_cast<uint16_t>(len - 1));
        }
    }

    // Dropping an HCI packet desynchronises the stack, so it must be visible --
    // just from a context where blocking on the UART is acceptable.
    {
        static uint32_t reported_drops = 0;
        const uint32_t drops = g_rx_dropped;
        if (drops != reported_drops) {
            printf("[BT] HCI rx ring full, dropped %lu packet(s)\n",
                   (unsigned long) (drops - reported_drops));
            reported_drops = drops;
        }
    }

    // And once more for anything sent by the last handler in the drain.
    emit_packet_sent_if_pending();

    // Timers and data sources. This is the cooperative equivalent of BTstack's
    // execute(); it returns immediately rather than owning the task.
    btstack_run_loop_embedded_execute_once();
}

} // namespace port
