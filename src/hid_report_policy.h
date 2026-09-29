#ifndef DS5_HID_REPORT_POLICY_H
#define DS5_HID_REPORT_POLICY_H

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ds5_hid {

constexpr std::size_t kInputReportBytes = 63;
constexpr std::size_t kBtHeaderBytes = 3;
constexpr std::size_t kBtReportBytes = kBtHeaderBytes + kInputReportBytes;
constexpr std::size_t kBtButtonPrefixBytes = kBtHeaderBytes + 10;

constexpr bool valid_polling_rate_mode(uint8_t mode) {
    return mode <= 3;
}

constexpr uint8_t gamepad_interval_ms(uint8_t mode) {
    return mode == 0 ? 4 : mode == 1 ? 2 : 1;
}

constexpr bool complete_bt_state_report(std::size_t length) {
    return length >= kBtReportBytes;
}

constexpr bool bt_packet_has_button_prefix(std::size_t length) {
    return length >= kBtButtonPrefixBytes;
}

inline bool update_cached_from_bt_packet(const uint8_t *packet, std::size_t length,
                                         uint8_t (&cached)[kInputReportBytes]) {
    if (!complete_bt_state_report(length)) return false;
    std::memcpy(cached, packet + kBtHeaderBytes, kInputReportBytes);
    return true;
}

inline bool copy_report_if_due(uint8_t mode, bool endpoint_ready, bool report_dirty,
                               const uint8_t (&cached)[kInputReportBytes],
                               uint8_t (&snapshot)[kInputReportBytes]) {
    if (!valid_polling_rate_mode(mode) || !endpoint_ready || (mode == 2 && !report_dirty)) return false;
    std::memcpy(snapshot, cached, kInputReportBytes);
    return true;
}

} // namespace ds5_hid

#endif // DS5_HID_REPORT_POLICY_H
