#pragma once

#include <cstddef>

namespace port {

// Matches the "config" entry in partitions_esp32s31.csv. Hard-coded rather than
// taken from SPI_FLASH_SEC_SIZE so a change to the partition table has to be
// made in both places deliberately.
constexpr size_t kConfigStorageSize = 4096;
constexpr size_t kConfigStoragePageSize = 256;

} // namespace port
