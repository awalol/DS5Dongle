//
// Wake-on-LAN over WiFi. See wol.h.
//
// Port to the 0.7.2 base: unlike the 0.6.0 version, we do NOT define
// tud_mount_cb/tud_umount_cb (wake.cpp already defines them with ENABLE_WAKE_HID).
// To tell whether the PC is on we query tud_mounted() during the observation
// window.
//

#include "wol.h"

#include <cstdio>
#include <cstring>

// WoL credentials live in secrets.h, which is gitignored (it holds the WiFi
// password and must never be committed). For a working WoL build, provide it by
// copying secrets.h.example to secrets.h and filling it in. When it is absent
// (CI builds, or anyone who has not configured WoL yet) fall back to inert
// placeholders so the firmware still compiles -- WoL simply will not wake
// anything until a real secrets.h is supplied.
#if __has_include("secrets.h")
#include "secrets.h"
#else
#warning "secrets.h not found: building with placeholder WoL credentials. Copy src/secrets.h.example to src/secrets.h and fill it in for Wake-on-LAN to work."
#define WIFI_SSID       "YOUR_SSID"
#define WIFI_PASS       "YOUR_WIFI_PASSWORD"
#define WOL_TARGET_MAC  "AA:BB:CC:DD:EE:FF"
#define WOL_PORT        9
#endif
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "tusb.h"
#include "wake.h"
#include "lwip/udp.h"
#include "lwip/ip_addr.h"

#ifdef WOL_UDP_LOG
#include "pico/stdio.h"
#include "pico/stdio/driver.h"
// DIAGNOSTIC: redirect all printf output to UDP broadcast packets
// (port WOL_UDP_LOG_PORT) so that a PC on the same network can capture the
// boot logs of the target PC without a UART adapter (the PC is powered off).
constexpr int WOL_UDP_LOG_PORT = 9999;
namespace {
udp_pcb     *g_log_pcb  = nullptr;
char         g_log_buf[256];
int          g_log_pos  = 0;
volatile bool g_log_busy = false;

void udp_log_flush() {
    if (g_log_pos == 0) return;
    if (g_log_pcb == nullptr) { g_log_pos = 0; return; }
    pbuf *p = pbuf_alloc(PBUF_TRANSPORT, g_log_pos, PBUF_RAM);
    if (p != nullptr) {
        memcpy(p->payload, g_log_buf, g_log_pos);
        ip_addr_t d; IP4_ADDR(&d, 255, 255, 255, 255);
        udp_sendto(g_log_pcb, p, &d, WOL_UDP_LOG_PORT);
        pbuf_free(p);
    }
    g_log_pos = 0;
}

void udp_log_out(const char *buf, int len) {
    if (g_log_busy) return;            // avoid recursion if lwIP prints
    g_log_busy = true;
    for (int i = 0; i < len; i++) {
        const char c = buf[i];
        if (c == '\r') continue;
        g_log_buf[g_log_pos++] = c;
        if (c == '\n' || g_log_pos >= static_cast<int>(sizeof(g_log_buf))) udp_log_flush();
    }
    g_log_busy = false;
}

stdio_driver_t g_udp_driver;
}  // namespace

static void udp_log_start() {
    if (g_log_pcb != nullptr) return;
    g_log_pcb = udp_new();
    if (g_log_pcb != nullptr) ip_set_option(g_log_pcb, SOF_BROADCAST);
    g_udp_driver.out_chars = udp_log_out;
    stdio_set_driver_enabled(&g_udp_driver, true);
    printf("[WoL][UDP-LOG] logging over WiFi active (broadcast:%d)\n", WOL_UDP_LOG_PORT);
}
#endif  // WOL_UDP_LOG

namespace {

// WoL sequence states.
enum class State {
    Idle,        // nothing to do
    Observe,     // controller connected: wait to see if the PC enumerates (=on)
    Connecting,  // waiting for association + IP via DHCP
    Backoff,     // short wait between connection retries
    Sending,     // sending the magic packets
    Cleanup      // bringing the WiFi down
};

State           state         = State::Idle;
absolute_time_t state_since;
absolute_time_t last_attempt;          // for debouncing
absolute_time_t observe_since;
absolute_time_t backoff_since;
absolute_time_t next_packet_time;
int             retries        = 0;
int             packets_sent   = 0;

// Observation window: if the PC is on, the USB device will be (or will
// become) mounted within this time after the controller connects -> no WoL
// needed. If it does not mount, we assume the PC is off and send.
constexpr int64_t HOST_OBSERVE_US    = 3'000'000;    // 3 s
constexpr int64_t CONNECT_TIMEOUT_US = 20'000'000;   // 20 s
constexpr int64_t DEBOUNCE_US        = 90'000'000;   // 90 s: covers the whole PC
                                                     // boot so that controller
                                                     // reconnections during boot do
                                                     // not retrigger WoL/WiFi
constexpr int64_t BACKOFF_US         = 1'500'000;    // 1.5 s between retries
constexpr int     MAX_RETRIES        = 2;
constexpr int     NUM_PACKETS        = 3;            // redundancy
constexpr int64_t PACKET_GAP_US      = 200'000;     // 200 ms between packets
// While the PC boots after the WoL, we suppress the controller's automatic
// power-off (wake.cpp) so it does not cut out mid-boot. Covers the whole boot.
constexpr int64_t POWEROFF_SUPPRESS_US = 180'000'000;  // 180 s

// Converts "AA:BB:CC:DD:EE:FF" (or with '-') to 6 bytes. Returns true if OK.
bool parse_mac(const char *str, uint8_t out[6]) {
    unsigned int v[6];
    int n = sscanf(str, "%2x:%2x:%2x:%2x:%2x:%2x",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    if (n != 6) {
        n = sscanf(str, "%2x-%2x-%2x-%2x-%2x-%2x",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    }
    if (n != 6) return false;
    for (int i = 0; i < 6; i++) out[i] = static_cast<uint8_t>(v[i]);
    return true;
}

void start_connect() {
    cyw43_arch_enable_sta_mode();
    if (cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASS, CYW43_AUTH_WPA2_AES_PSK) != 0) {
        printf("[WoL] connect_async could not be started\n");
        cyw43_arch_disable_sta_mode();
        state = State::Idle;
        return;
    }
    state_since = get_absolute_time();
    state = State::Connecting;
}

void send_magic_packet() {
    uint8_t mac[6];
    if (!parse_mac(WOL_TARGET_MAC, mac)) {
        printf("[WoL] invalid MAC in secrets.h: %s\n", WOL_TARGET_MAC);
        return;
    }

    // Magic packet: 6x 0xFF + 16x the target MAC = 102 bytes.
    uint8_t magic[6 + 16 * 6];
    memset(magic, 0xFF, 6);
    for (int i = 0; i < 16; i++) {
        memcpy(magic + 6 + i * 6, mac, 6);
    }

    udp_pcb *pcb = udp_new();
    if (pcb == nullptr) {
        printf("[WoL] udp_new failed\n");
        return;
    }
    ip_set_option(pcb, SOF_BROADCAST);

    pbuf *p = pbuf_alloc(PBUF_TRANSPORT, sizeof(magic), PBUF_RAM);
    if (p == nullptr) {
        udp_remove(pcb);
        return;
    }
    memcpy(p->payload, magic, sizeof(magic));

    ip_addr_t dest;
    IP4_ADDR(&dest, 255, 255, 255, 255);   // broadcast limited to the local subnet

    const err_t e = udp_sendto(pcb, p, &dest, WOL_PORT);
    pbuf_free(p);
    udp_remove(pcb);

    if (e != ERR_OK) {
        printf("[WoL] udp_sendto error=%d\n", e);
    } else {
        printf("[WoL] Magic packet sent to %s (port %d)\n", WOL_TARGET_MAC, WOL_PORT);
    }
}

}  // namespace

void wol_init() {
    state = State::Idle;
    last_attempt = nil_time;
    retries = 0;
    packets_sent = 0;
}

void wol_request() {
    if (state != State::Idle) {
        return;  // a sequence is already in progress
    }
    if (!is_nil_time(last_attempt) &&
        absolute_time_diff_us(last_attempt, get_absolute_time()) < DEBOUNCE_US) {
        return;  // debounce: too soon since the last WoL
    }
    last_attempt = get_absolute_time();
    retries = 0;
    observe_since = get_absolute_time();
    state = State::Observe;
    // The controller just connected and we may wake the PC: prevent the
    // sustained-suspend power-off from cutting the controller while the PC boots.
    wake_suppress_poweroff(POWEROFF_SUPPRESS_US);
    printf("[WoL] Controller connected; observing whether the PC enumerates (%lld ms)\n",
           static_cast<long long>(HOST_OBSERVE_US / 1000));
}

void wol_tick() {
    switch (state) {
        case State::Idle:
            return;

        case State::Observe:
#ifdef WOL_FORCE_TEST
            // TEST MODE: ignore the PC-on gating and always bring up the WiFi,
            // to validate the whole path (association + IP + magic packet) with the
            // development PC powered on. Do NOT use in production.
            printf("[WoL] (TEST) forcing WoL: ignoring the PC gating and bringing up WiFi\n");
            start_connect();
            return;
#else
            // The PC is only considered "on" if the USB device is mounted
            // AND the bus is ACTIVE (not suspended). When the PC powers off (S5) but the
            // port still supplies standby power, the bus stays SUSPENDED and tud_mounted()
            // remains true -> that's why tud_mounted() alone is not enough: we must
            // also require !tud_suspended(). So:
            //   - PC on and active       -> mounted && !suspended -> abort WoL
            //   - PC asleep S3           -> USB remote wakeup (wake.cpp) wakes it
            //                                within the window -> becomes active -> abort
            //   - PC off S5 (standby)    -> stays suspended/unmounted -> fire WoL
            if (tud_mounted() && !tud_suspended()) {
                printf("[WoL] USB host active (PC on): WoL aborted\n");
                // No wake is being sent, so release the power-off suppression armed
                // on connect -- otherwise a later genuine sleep within the window
                // would skip the controller's battery power-off.
                wake_cancel_poweroff_suppress();
                state = State::Idle;
                return;
            }
            if (absolute_time_diff_us(observe_since, get_absolute_time()) > HOST_OBSERVE_US) {
                printf("[WoL] The PC seems off/suspended; bringing up WiFi for the magic packet\n");
                start_connect();
            }
            return;
#endif

        case State::Connecting: {
            const int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
            if (link == CYW43_LINK_UP) {
                printf("[WoL] WiFi has IP; sending magic packets\n");
#ifdef WOL_UDP_LOG
                udp_log_start();
#endif
                packets_sent = 0;
                next_packet_time = get_absolute_time();
                state = State::Sending;
                return;
            }
            if (link == CYW43_LINK_BADAUTH) {
                // Permanent error: wrong credentials. Retrying won't help.
                printf("[WoL] Wrong WiFi credentials (BADAUTH); aborting\n");
                state = State::Cleanup;
                return;
            }
            const bool failed = (link < 0);  // FAIL / NONET (may be transient)
            const bool timed_out =
                absolute_time_diff_us(state_since, get_absolute_time()) > CONNECT_TIMEOUT_US;
            if (failed || timed_out) {
                printf("[WoL] connection failed/timeout (link=%d, retry=%d)\n", link, retries);
                if (retries < MAX_RETRIES) {
                    retries++;
                    backoff_since = get_absolute_time();
                    state = State::Backoff;   // short wait before retrying
                } else {
                    state = State::Cleanup;
                }
            }
            return;
        }

        case State::Backoff:
            if (absolute_time_diff_us(backoff_since, get_absolute_time()) > BACKOFF_US) {
                start_connect();   // keeps STA enabled; new association attempt
            }
            return;

        case State::Sending:
            if (absolute_time_diff_us(get_absolute_time(), next_packet_time) <= 0) {
                send_magic_packet();
                packets_sent++;
                next_packet_time = delayed_by_us(get_absolute_time(), PACKET_GAP_US);
                if (packets_sent >= NUM_PACKETS) {
#ifdef WOL_UDP_LOG
                    // We keep the WiFi up to keep sending logs during the target PC's
                    // boot (to capture the BT disconnection and its reason).
                    printf("[WoL][UDP-LOG] packets sent; keeping WiFi up to capture the boot\n");
                    state = State::Idle;
#else
                    state = State::Cleanup;
#endif
                }
            }
            return;

        case State::Cleanup:
            printf("[WoL] Bringing the WiFi down\n");
            cyw43_arch_disable_sta_mode();
            state = State::Idle;
            return;
    }
}
