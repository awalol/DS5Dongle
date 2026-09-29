#include "hid_report_policy.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

static void require(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

int main() {
    for (uint8_t mode = 0; mode <= 3; ++mode) {
        require(ds5_hid::valid_polling_rate_mode(mode), "mode 0-3 valid");
    }
    require(!ds5_hid::valid_polling_rate_mode(4), "mode 4 invalid");
    require(ds5_hid::gamepad_interval_ms(0) == 4, "250 Hz interval");
    require(ds5_hid::gamepad_interval_ms(1) == 2, "500 Hz interval");
    require(ds5_hid::gamepad_interval_ms(2) == 1, "real-time interval");
    require(ds5_hid::gamepad_interval_ms(3) == 1, "1000 Hz fixed interval");

    uint8_t cached[ds5_hid::kInputReportBytes]{};
    uint8_t snapshot[ds5_hid::kInputReportBytes]{};
    uint8_t packet[ds5_hid::kBtReportBytes]{};
    packet[0] = 0xa2;
    packet[1] = 0x31;
    packet[2] = 0x01;
    for (size_t i = 0; i < ds5_hid::kInputReportBytes; ++i) {
        packet[i + ds5_hid::kBtHeaderBytes] = static_cast<uint8_t>(i + 1);
    }

    require(!ds5_hid::complete_bt_state_report(ds5_hid::kBtReportBytes - 1), "short BT state rejected");
    require(ds5_hid::complete_bt_state_report(ds5_hid::kBtReportBytes), "66-byte BT state accepted");
    require(!ds5_hid::bt_packet_has_button_prefix(12), "short BT packet cannot enter inactivity check");
    require(ds5_hid::bt_packet_has_button_prefix(13), "13-byte BT packet has all button fields");
    require(!ds5_hid::update_cached_from_bt_packet(packet, ds5_hid::kBtReportBytes - 1, cached), "short BT packet cannot update cache");
    for (uint8_t value : cached) require(value == 0, "short packet leaves cache unchanged");
    require(ds5_hid::update_cached_from_bt_packet(packet, ds5_hid::kBtReportBytes, cached), "complete BT packet updates cache");

    for (uint8_t mode : {uint8_t{0}, uint8_t{1}, uint8_t{3}}) {
        require(ds5_hid::copy_report_if_due(mode, true, false, cached, snapshot), "fixed mode sends without new BT");
        require(std::memcmp(cached, snapshot, sizeof(cached)) == 0, "fixed mode copies full 63 bytes");
        std::memset(snapshot, 0, sizeof(snapshot));
        require(ds5_hid::copy_report_if_due(mode, true, false, cached, snapshot), "fixed mode repeats cached state");
        require(std::memcmp(cached, snapshot, sizeof(cached)) == 0, "repeat keeps sensor bytes and timestamp");
    }

    std::memset(snapshot, 0xa5, sizeof(snapshot));
    require(!ds5_hid::copy_report_if_due(3, false, true, cached, snapshot), "busy endpoint cannot queue");
    for (uint8_t value : snapshot) require(value == 0xa5, "busy endpoint leaves destination untouched");

    packet[ds5_hid::kBtHeaderBytes + 27] = 0x91; // SensorTimestamp byte remains an opaque input value.
    packet[ds5_hid::kBtHeaderBytes + 15] = 0x42; // Angular velocity byte likewise remains unchanged.
    require(ds5_hid::update_cached_from_bt_packet(packet, sizeof(packet), cached), "new BT state replaces cache");
    require(ds5_hid::copy_report_if_due(3, true, false, cached, snapshot), "next ready slot takes latest state");
    require(snapshot[27] == 0x91 && snapshot[15] == 0x42, "sensor fields are not synthesized");

    require(!ds5_hid::copy_report_if_due(2, true, false, cached, snapshot), "real-time needs dirty input");
    require(ds5_hid::copy_report_if_due(2, true, true, cached, snapshot), "real-time sends a new input");
    require(std::memcmp(cached, snapshot, sizeof(cached)) == 0, "real-time sends latest full state");
    require(!ds5_hid::copy_report_if_due(2, false, true, cached, snapshot), "real-time retains dirty state while busy");

    // The disconnect callback supplies this same 3+63 shape with neutral payload.
    std::memset(packet + ds5_hid::kBtHeaderBytes, 0, ds5_hid::kInputReportBytes);
    require(ds5_hid::update_cached_from_bt_packet(packet, sizeof(packet), cached), "disconnect state updates cache");
    require(ds5_hid::copy_report_if_due(3, true, false, cached, snapshot), "fixed mode repeats neutral disconnect state");
    for (uint8_t value : snapshot) require(value == 0, "disconnect snapshot is neutral");

    std::puts("PASS hid_report_policy: four modes, complete BT packets, busy endpoint, latest snapshot, and sensor bytes");
    return 0;
}
