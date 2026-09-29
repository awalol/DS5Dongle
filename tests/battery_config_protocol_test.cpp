#include "battery_config_protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        std::exit(1); \
    } \
} while (0)

namespace {
uint32_t reference_crc32(const uint8_t *data, size_t size) {
    uint32_t crc = ~0xEADA2D49u;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return ~crc;
}
}

int main() {
    namespace p = battery_config_protocol;
    static_assert(p::kRecordSize == 12);
    static_assert(p::kTailSize == 5);

    std::array<uint8_t, p::kRecordSize> record{};
    p::encode_record(record.data(), true, reference_crc32);
    // Hand-checked BTF1 format 1, bit0 enabled, CRC32 seed 0xEADA2D49.
    constexpr std::array<uint8_t, 12> enabled_record{
        0x42, 0x54, 0x46, 0x31, 0x01, 0x01, 0x00, 0x00,
        0xE5, 0xC3, 0x47, 0x6C,
    };
    REQUIRE(record == enabled_record);

    bool enabled = false;
    REQUIRE(p::decode_record(record.data(), enabled, reference_crc32) && enabled);
    p::encode_record(record.data(), false, reference_crc32);
    constexpr std::array<uint8_t, 12> disabled_record{
        0x42, 0x54, 0x46, 0x31, 0x01, 0x00, 0x00, 0x00,
        0xD2, 0xA9, 0x85, 0x6D,
    };
    REQUIRE(record == disabled_record);
    REQUIRE(p::decode_record(record.data(), enabled, reference_crc32) && !enabled);

    record.fill(0xFF);
    enabled = true;
    REQUIRE(!p::decode_record(record.data(), enabled, reference_crc32) && !enabled);
    record = enabled_record;
    record[8] ^= 0x01;
    REQUIRE(!p::decode_record(record.data(), enabled, reference_crc32) && !enabled);
    record = enabled_record;
    record[4] = 2;
    REQUIRE(!p::decode_record(record.data(), enabled, reference_crc32) && !enabled);
    record = enabled_record;
    record[5] = 2;
    REQUIRE(!p::decode_record(record.data(), enabled, reference_crc32) && !enabled);
    record = enabled_record;
    record[6] = 1;
    REQUIRE(!p::decode_record(record.data(), enabled, reference_crc32) && !enabled);

    std::array<uint8_t, 63> command{};
    command[0] = 0x05;
    command[1] = 0x42;
    command[2] = 0x46;
    command[3] = 0x01;
    command[4] = 1;
    REQUIRE(p::decode_update(command.data(), command.size(), enabled) && enabled);
    command[5] = 1;
    REQUIRE(!p::decode_update(command.data(), command.size(), enabled));
    command[5] = 0;
    command[4] = 2;
    REQUIRE(!p::decode_update(command.data(), command.size(), enabled));
    command[4] = 1;
    command[0] = 4; // BOOTSEL is reserved and must not update the flag.
    REQUIRE(!p::decode_update(command.data(), command.size(), enabled));
    command[0] = 5;
    REQUIRE(!p::decode_update(command.data(), 4, enabled));

    std::array<uint8_t, p::kTailSize> tail{};
    p::encode_tail(tail.data(), true, 1);
    REQUIRE((tail == std::array<uint8_t, 5>{0x42, 0x46, 0x01, 0x01, 0x01}));
    p::encode_tail(tail.data(), false, 0);
    REQUIRE((tail == std::array<uint8_t, 5>{0x42, 0x46, 0x01, 0x00, 0x00}));
}
