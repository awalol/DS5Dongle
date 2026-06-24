//
// Configuració mínima de lwIP per a la funció Wake-on-LAN (ENABLE_WOL).
//
// Mode poll (NO_SYS=1), compatible amb pico_cyw43_arch_lwip_poll i amb la
// coexistència WiFi + BTstack sobre el mateix xip CYW43.
//
// Derivat de l'exemple oficial pico-examples (lwipopts_examples_common.h),
// amb el broadcast UDP habilitat per poder enviar el magic packet.
//

#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

// Sistema sense RTOS: lwIP es serveix des de cyw43_arch_poll() al bucle principal.
#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    4000
#define MEMP_NUM_ARP_QUEUE          10
// El WoL nomes envia 1 PBUF UDP de 102 bytes; el pool nomes l'usa la RX del
// driver CYW43 (DHCP/ARP), que cap en pocs buffers. 8 estalvia ~24 KB respecte 24.
#define PBUF_POOL_SIZE              8
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
// TCP_MSS es defineix incondicionalment a lwIP i, encara amb LWIP_TCP=0, es el
// que dimensiona PBUF_POOL_BUFSIZE (~1516 B/buffer). El mantenim a 1460.
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
// Nomes cal UDP (magic packet + DHCP). TCP desactivat: estalvia pools/PCB + flash.
#define LWIP_TCP                    0
#define LWIP_UDP                    1
#define LWIP_DNS                    0
#define LWIP_NETIF_TX_SINGLE_PBUF   1
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0

// Necessari per poder enviar el magic packet com a broadcast UDP.
#define IP_SOF_BROADCAST            1
#define IP_SOF_BROADCAST_RECV       1

#endif // _LWIPOPTS_H
