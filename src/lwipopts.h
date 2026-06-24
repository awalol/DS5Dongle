//
// Minimal lwIP configuration for the Wake-on-LAN feature (ENABLE_WOL).
//
// Poll mode (NO_SYS=1), compatible with pico_cyw43_arch_lwip_poll and with
// WiFi + BTstack coexistence on the same CYW43 chip.
//
// Derived from the official pico-examples sample (lwipopts_examples_common.h),
// with UDP broadcast enabled so we can send the magic packet.
//

#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

// No-RTOS system: lwIP is serviced from cyw43_arch_poll() in the main loop.
#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    4000
#define MEMP_NUM_ARP_QUEUE          10
// WoL only sends a single 102-byte UDP PBUF; the pool is only used by the CYW43
// driver RX path (DHCP/ARP), which fits in a few buffers. 8 saves ~24 KB vs 24.
#define PBUF_POOL_SIZE              8
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
// TCP_MSS is defined unconditionally in lwIP and, even with LWIP_TCP=0, it is what
// sizes PBUF_POOL_BUFSIZE (~1516 B/buffer). We keep it at 1460.
#define TCP_MSS                     1460
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0
#define LWIP_CHKSUM_ALGORITHM       3
#define LWIP_DHCP                   1
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
// Only UDP is needed (magic packet + DHCP). TCP disabled: saves pools/PCB + flash.
#define LWIP_TCP                    0
#define LWIP_UDP                    1
#define LWIP_DNS                    0
#define LWIP_NETIF_TX_SINGLE_PBUF   1
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0

// Required to send the magic packet as a UDP broadcast.
#define IP_SOF_BROADCAST            1
#define IP_SOF_BROADCAST_RECV       1

#endif // _LWIPOPTS_H
