//
// DualSense Edge profile support. See dse.h for the protocol overview.
//

#include "port/port.h"
#include "dse.h"
#include "bt.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <unordered_map>

// Provided by bt.cpp
extern std::unordered_map<uint8_t, std::vector<uint8_t> > feature_data;
uint16_t bt_control_cid();          // current HID control channel id (0 if none)
void bt_control_send(const uint8_t *data, uint16_t len);

// Unlock state: 0 = idle, 1 = waiting for the controller to process SET 0x80.
static int unlock_phase = 0;
static uint32_t unlock_started_ms = 0;

// False from connect until the unlock + prefetch completes. While false the
// USB GET handler NAKs profile reads so the PS app retries (it polls these
// repeatedly during load) instead of caching an empty snapshot. This avoids
// any USB enumeration delay.
static bool profiles_ready = true;

// Post-save snapshot regeneration tracking.
static uint32_t profile_written_ms = 0;
static int post_save_round = 0;

// Paced profile prefetch sequencer. Fetching all 12 profile reports as a
// burst overflows the L2CAP control channel and silently drops requests
// (observed as one profile showing "not assigned"). One GET per 80ms is
// reliable.
static uint8_t prefetch_next = 0;       // 0 = idle, else next report id
static uint32_t prefetch_last_ms = 0;
static bool prefetch_mark_ready = false; // set profiles_ready when sequence ends

static void prefetch_start(bool mark_ready_after) {
    prefetch_next = 0x70;
    prefetch_last_ms = 0;
    prefetch_mark_ready = mark_ready_after;
}

bool dse_is_profile_report(uint8_t reportId) {
    return reportId >= 0x70 && reportId <= 0x7B;
}

bool dse_profiles_ready() {
    return profiles_ready;
}

void dse_on_connect() {
    const uint16_t cid = bt_control_cid();
    // 1) SET 0x65: verbatim echo of the 0x20 firmware report body (native
    //    sends 63 bytes, no CRC recompute).
    auto it = feature_data.find(0x20);
    if (cid != 0 && it != feature_data.end() && it->second.size() >= 62) {
        const auto &fw = it->second;
        uint8_t handshake[63];
        handshake[0] = 0x53;
        handshake[1] = 0x65;
        memcpy(handshake + 2, fw.data(), 61);
        bt_control_send(handshake, sizeof(handshake));
    }
    // 2) SET 0x80 {0x70,0x01,...}: profile unlock. Must carry a valid CRC32
    //    trailer or the controller rejects it with HANDSHAKE 0x04
    //    (ERR_INVALID_PARAMETER). set_feature_data() appends the checksum.
    uint8_t unlock[59]{};
    unlock[0] = 0x70;
    unlock[1] = 0x01;
    set_feature_data(0x80, unlock, sizeof(unlock));
    // 3) Caller connects USB immediately. Gate profile reads until ready.
    unlock_started_ms = port::now_ms();
    unlock_phase = 1;
    profiles_ready = false;
}

void dse_on_control_packet(const uint8_t *packet, uint16_t size) {
    // 1-byte HID HANDSHAKE in response to a SET command:
    // 0x00 = SUCCESS, 0x04 = ERR_INVALID_PARAMETER (command rejected).
    if (size == 1 && (packet[0] & 0xF0) == 0x00) {
        printf("[DSE] HID HANDSHAKE: 0x%02X (%s)\n", packet[0],
               packet[0] == 0x00 ? "success" : "rejected");
    }
}

void dse_on_profile_write(uint8_t reportId) {
    if (reportId >= 0x60 && reportId <= 0x62) {
        profile_written_ms = port::now_ms();
        post_save_round = 0;
    }
}

void dse_task() {
    const uint32_t now = port::now_ms();
    const uint16_t cid = bt_control_cid();

    // Paced prefetch sequencer: one profile GET per 80ms.
    if (prefetch_next != 0 && cid != 0) {
        if (now - prefetch_last_ms >= 80) {
            prefetch_last_ms = now;
            get_feature_data(prefetch_next, 64);
            if (prefetch_next == 0x7B) {
                prefetch_next = 0;
                if (prefetch_mark_ready) {
                    prefetch_mark_ready = false;
                    profiles_ready = true;
                    printf("[DSE] Profile snapshot ready\n");
                }
            } else {
                prefetch_next++;
            }
        }
    } else if (prefetch_next != 0) {
        prefetch_next = 0; // controller gone
        prefetch_mark_ready = false;
    }

    // Post-save snapshot regeneration, mirroring the PS app's own cycle:
    //   SET 0x80  ->  poll GET 0x81 (x6, spaced)  ->  re-read profiles.
    // A profile write updates controller storage but not the read-back
    // snapshot; without this the save is only visible after the next app open.
    if (profile_written_ms != 0 && cid != 0) {
        const uint32_t since_write = now - profile_written_ms;
        if (post_save_round == 0 && since_write >= 500) {
            uint8_t unlock[59]{};
            unlock[0] = 0x70;
            unlock[1] = 0x01;
            set_feature_data(0x80, unlock, sizeof(unlock));
            post_save_round = 1;
            printf("[DSE] Post-save: re-sent 0x80\n");
        } else if (post_save_round >= 1 && post_save_round <= 6 &&
                   since_write >= 1000 + 250u * (post_save_round - 1)) {
            get_feature_data(0x81, 64); // status poll, mirrors app behavior
            post_save_round++;
        } else if (post_save_round == 7 && since_write >= 5500) {
            prefetch_start(false); // paced refetch of 0x70-0x7B
            post_save_round = 8;
            profile_written_ms = 0;
            printf("[DSE] Post-save: profile snapshot refetch started\n");
        }
    }

    // Unlock wait: once the controller has had ~4s to prepare the snapshot,
    // prefetch it (paced). USB is already connected; completion flips the
    // profiles_ready gate so the app's retry reads return real data.
    if (unlock_phase == 0) {
        return;
    }
    if (cid == 0) { // controller went away mid-unlock
        unlock_phase = 0;
        profiles_ready = true; // don't leave profiles gated if it vanished
        return;
    }
    if (unlock_phase == 1 && now - unlock_started_ms >= 4000) {
        prefetch_start(true);
        unlock_phase = 0;
        printf("[DSE] Unlock wait done, prefetching profile reports\n");
    }
}
