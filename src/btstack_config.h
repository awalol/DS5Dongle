// https://github.com/rafaelvaloto/Pico_W-Dualsense/blob/main/btstack_config.h

// c
#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

#ifndef ENABLE_CLASSIC
#define ENABLE_CLASSIC
#endif


// CYW43 HCI Transport requires pre-buffer space for packet header

// Se estiver 1 ou 2, o 0x31 do DualSense causa estouro
#define MAX_NR_HCI_ACL_PACKETS 4

#define MAX_NR_HCI_CONNECTIONS 1
#define MAX_NR_L2CAP_CHANNELS  2
#define MAX_NR_L2CAP_SERVICES  3 // GDP + CONTROL + INTERRUPT
//
#define HCI_ACL_PAYLOAD_SIZE 1021
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4


#define MAX_NR_RFCOMM_MULTIPLEXERS 0
#define MAX_NR_RFCOMM_SERVICES 0
#define MAX_NR_RFCOMM_CHANNELS 0

// CYW43 específico - necessário para o transport layer

#define NVM_NUM_LINK_KEYS 4
#define NVM_NUM_DEVICE_DB_ENTRIES 4
#define HAVE_EMBEDDED_TIME_MS

// Logging
// Release perf: ENABLE_LOG_INFO dropped -- it pulled btstack's internal per-event
// log_info() strings + formatting (flash/XIP pressure + CPU on the BT event path)
// that the release never needs. ENABLE_LOG_ERROR kept so genuine error paths still
// report. ENABLE_PRINTF_HEXDUMP MUST stay: the Pico SDK unconditionally compiles
// btstack's hci_dump_embedded_stdout.c, which has a hard #error without it. It
// costs nothing in the shipped binary -- hci_dump is never initialized so that
// code (and printf_hexdump, only called under ENABLE_VERBOSE=OFF) is gc-collected.
#define ENABLE_PRINTF_HEXDUMP
#define ENABLE_LOG_ERROR

#endif
