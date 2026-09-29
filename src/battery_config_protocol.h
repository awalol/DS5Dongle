#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Byte-oriented wire/flash encoding. Config_body remains the original v5/22-byte
// struct; this record lives after Config in the same flash program page.
namespace battery_config_protocol {
inline constexpr size_t kRecordSize = 12;
inline constexpr size_t kTailSize = 5;
inline constexpr size_t kSetPayloadSize = 63;
using Crc32 = uint32_t (*)(const uint8_t *, size_t);

inline void encode_record(uint8_t *record, bool enabled, Crc32 crc32_fn) {
    record[0] = 'B';
    record[1] = 'T';
    record[2] = 'F';
    record[3] = '1';
    record[4] = 1;
    record[5] = enabled ? 1 : 0;
    record[6] = 0;
    record[7] = 0;
    const uint32_t crc = crc32_fn(record, 8);
    for (unsigned i = 0; i < 4; ++i) {
        record[8 + i] = static_cast<uint8_t>(crc >> (8 * i));
    }
}

inline bool decode_record(const uint8_t *record, bool &enabled, Crc32 crc32_fn) {
    enabled = false;
    if (std::memcmp(record, "BTF1", 4) != 0 || record[4] != 1 ||
        (record[5] & ~1u) != 0 || record[6] != 0 || record[7] != 0) {
        return false;
    }
    uint32_t stored_crc = 0;
    for (unsigned i = 0; i < 4; ++i) {
        stored_crc |= static_cast<uint32_t>(record[8 + i]) << (8 * i);
    }
    if (stored_crc != crc32_fn(record, 8)) {
        return false;
    }
    enabled = (record[5] & 1u) != 0;
    return true;
}

inline bool decode_update(const uint8_t *payload, size_t size, bool &enabled) {
    if (size < 5 || size > kSetPayloadSize || payload[0] != 0x05 ||
        payload[1] != 'B' || payload[2] != 'F' || payload[3] != 1 ||
        (payload[4] & ~1u) != 0) {
        return false;
    }
    for (size_t i = 5; i < size; ++i) {
        if (payload[i] != 0) {
            return false;
        }
    }
    enabled = (payload[4] & 1u) != 0;
    return true;
}

inline void encode_tail(uint8_t *tail, bool enabled, uint8_t save_status) {
    tail[0] = 'B';
    tail[1] = 'F';
    tail[2] = 1;
    tail[3] = enabled ? 1 : 0;
    tail[4] = save_status;
}
} // namespace battery_config_protocol
