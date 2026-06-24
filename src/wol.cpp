//
// Wake-on-LAN sobre WiFi. Vegeu wol.h.
//
// Port a la base 0.7.2: a diferència de la versió 0.6.0, NO definim
// tud_mount_cb/tud_umount_cb (wake.cpp ja els defineix amb ENABLE_WAKE_HID).
// Per saber si el PC està encès consultem tud_mounted() durant la finestra
// d'observació.
//

#include "wol.h"

#include <cstdio>
#include <cstring>

#include "secrets.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "tusb.h"
#include "wake.h"
#include "lwip/udp.h"
#include "lwip/ip_addr.h"

#ifdef WOL_UDP_LOG
#include "pico/stdio.h"
#include "pico/stdio/driver.h"
// DIAGNOSTIC: redirigeix tota la sortida de printf cap a paquets UDP broadcast
// (port WOL_UDP_LOG_PORT) perque un PC de la mateixa xarxa pugui capturar els
// logs de l'arrencada del PC objectiu sense adaptador UART (el PC esta apagat).
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
    if (g_log_busy) return;            // evita recursio si lwIP imprimis
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
    printf("[WoL][UDP-LOG] log per WiFi actiu (broadcast:%d)\n", WOL_UDP_LOG_PORT);
}
#endif  // WOL_UDP_LOG

namespace {

// Estats de la seqüència de WoL.
enum class State {
    Idle,        // res a fer
    Observe,     // comandament connectat: esperem a veure si el PC enumera (=encès)
    Connecting,  // esperant associació + IP per DHCP
    Backoff,     // espera curta entre reintents de connexió
    Sending,     // enviant els magic packets
    Cleanup      // baixant el WiFi
};

State           state         = State::Idle;
absolute_time_t state_since;
absolute_time_t last_attempt;          // per a l'anti-rebot
absolute_time_t observe_since;
absolute_time_t backoff_since;
absolute_time_t next_packet_time;
int             retries        = 0;
int             packets_sent   = 0;

// Finestra d'observació: si el PC està encès, el dispositiu USB estarà (o
// passarà a estar) muntat dins d'aquest temps després de connectar el
// comandament -> NO cal WoL. Si no es munta, assumim PC apagat i enviem.
constexpr int64_t HOST_OBSERVE_US    = 3'000'000;    // 3 s
constexpr int64_t CONNECT_TIMEOUT_US = 20'000'000;   // 20 s
constexpr int64_t DEBOUNCE_US        = 90'000'000;   // 90 s: cobreix tot l'arrencada
                                                     // del PC perque les reconnexions
                                                     // del comandament durant el boot no
                                                     // tornin a disparar WoL/WiFi
constexpr int64_t BACKOFF_US         = 1'500'000;    // 1.5 s entre reintents
constexpr int     MAX_RETRIES        = 2;
constexpr int     NUM_PACKETS        = 3;            // redundància
constexpr int64_t PACKET_GAP_US      = 200'000;     // 200 ms entre paquets
// Mentre el PC arrenca despres del WoL, suprimim l'apagat automatic del
// comandament (wake.cpp) perque no es talli a mig boot. Cobreix tota l'arrencada.
constexpr int64_t POWEROFF_SUPPRESS_US = 180'000'000;  // 180 s

// Converteix "AA:BB:CC:DD:EE:FF" (o amb '-') a 6 bytes. Retorna true si OK.
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
        printf("[WoL] connect_async no s'ha pogut iniciar\n");
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
        printf("[WoL] MAC invàlida a secrets.h: %s\n", WOL_TARGET_MAC);
        return;
    }

    // Magic packet: 6x 0xFF + 16x la MAC del destí = 102 bytes.
    uint8_t magic[6 + 16 * 6];
    memset(magic, 0xFF, 6);
    for (int i = 0; i < 16; i++) {
        memcpy(magic + 6 + i * 6, mac, 6);
    }

    udp_pcb *pcb = udp_new();
    if (pcb == nullptr) {
        printf("[WoL] udp_new ha fallat\n");
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
    IP4_ADDR(&dest, 255, 255, 255, 255);   // broadcast limitat a la subxarxa local

    const err_t e = udp_sendto(pcb, p, &dest, WOL_PORT);
    pbuf_free(p);
    udp_remove(pcb);

    if (e != ERR_OK) {
        printf("[WoL] udp_sendto error=%d\n", e);
    } else {
        printf("[WoL] Magic packet enviat a %s (port %d)\n", WOL_TARGET_MAC, WOL_PORT);
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
        return;  // ja hi ha una seqüència en curs
    }
    if (!is_nil_time(last_attempt) &&
        absolute_time_diff_us(last_attempt, get_absolute_time()) < DEBOUNCE_US) {
        return;  // anti-rebot: massa aviat des de l'últim WoL
    }
    last_attempt = get_absolute_time();
    retries = 0;
    observe_since = get_absolute_time();
    state = State::Observe;
    // El comandament acaba de connectar i potser despertarem el PC: evita que
    // l'apagat per suspensio sostinguda talli el comandament mentre el PC arrenca.
    wake_suppress_poweroff(POWEROFF_SUPPRESS_US);
    printf("[WoL] Comandament connectat; observant si el PC enumera (%lld ms)\n",
           static_cast<long long>(HOST_OBSERVE_US / 1000));
}

void wol_tick() {
    switch (state) {
        case State::Idle:
            return;

        case State::Observe:
#ifdef WOL_FORCE_TEST
            // MODE DE PROVA: ignora el gating del PC encès i puja el WiFi sempre,
            // per validar tot el camí (associació + IP + magic packet) amb el PC
            // de desenvolupament encès. NO usar en producció.
            printf("[WoL] (TEST) forçant WoL: ignoro el gating del PC i pujo WiFi\n");
            start_connect();
            return;
#else
            // El PC nomes es considera "ences" si el dispositiu USB esta muntat
            // I el bus esta ACTIU (no suspes). Quan el PC s'apaga (S5) pero el port
            // segueix donant corrent standby, el bus queda SUSPES i tud_mounted()
            // continua true -> per aixo NO n'hi ha prou amb tud_mounted(): cal
            // exigir tambe !tud_suspended(). Aixi:
            //   - PC ences i actiu        -> mounted && !suspended -> avortar WoL
            //   - PC adormit S3           -> el USB remote wakeup (wake.cpp) el desperta
            //                                dins la finestra -> passa a actiu -> avortar
            //   - PC apagat S5 (standby)  -> queda suspes/desmuntat -> disparar WoL
            if (tud_mounted() && !tud_suspended()) {
                printf("[WoL] Host USB actiu (PC ences): WoL avortat\n");
                state = State::Idle;
                return;
            }
            if (absolute_time_diff_us(observe_since, get_absolute_time()) > HOST_OBSERVE_US) {
                printf("[WoL] El PC sembla apagat/suspes; pujant WiFi per al magic packet\n");
                start_connect();
            }
            return;
#endif

        case State::Connecting: {
            const int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
            if (link == CYW43_LINK_UP) {
                printf("[WoL] WiFi amb IP; enviant magic packets\n");
#ifdef WOL_UDP_LOG
                udp_log_start();
#endif
                packets_sent = 0;
                next_packet_time = get_absolute_time();
                state = State::Sending;
                return;
            }
            if (link == CYW43_LINK_BADAUTH) {
                // Error permanent: credencials incorrectes. Reintentar no ajuda.
                printf("[WoL] Credencials WiFi incorrectes (BADAUTH); s'avorta\n");
                state = State::Cleanup;
                return;
            }
            const bool failed = (link < 0);  // FAIL / NONET (poden ser transitoris)
            const bool timed_out =
                absolute_time_diff_us(state_since, get_absolute_time()) > CONNECT_TIMEOUT_US;
            if (failed || timed_out) {
                printf("[WoL] connexió fallida/timeout (link=%d, retry=%d)\n", link, retries);
                if (retries < MAX_RETRIES) {
                    retries++;
                    backoff_since = get_absolute_time();
                    state = State::Backoff;   // espera curta abans de reintentar
                } else {
                    state = State::Cleanup;
                }
            }
            return;
        }

        case State::Backoff:
            if (absolute_time_diff_us(backoff_since, get_absolute_time()) > BACKOFF_US) {
                start_connect();   // manté STA activat; nou intent d'associació
            }
            return;

        case State::Sending:
            if (absolute_time_diff_us(get_absolute_time(), next_packet_time) <= 0) {
                send_magic_packet();
                packets_sent++;
                next_packet_time = delayed_by_us(get_absolute_time(), PACKET_GAP_US);
                if (packets_sent >= NUM_PACKETS) {
#ifdef WOL_UDP_LOG
                    // Mantenim el WiFi amunt per seguir enviant logs durant l'arrencada
                    // del PC objectiu (per capturar la desconnexio del BT i el seu motiu).
                    printf("[WoL][UDP-LOG] paquets enviats; mantinc WiFi amunt per capturar el boot\n");
                    state = State::Idle;
#else
                    state = State::Cleanup;
#endif
                }
            }
            return;

        case State::Cleanup:
            printf("[WoL] Baixant el WiFi\n");
            cyw43_arch_disable_sta_mode();
            state = State::Idle;
            return;
    }
}
