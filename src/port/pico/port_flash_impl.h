#pragma once

#include <cstddef>

#include "hardware/flash.h"

namespace port {

constexpr size_t kConfigStorageSize = FLASH_SECTOR_SIZE;
constexpr size_t kConfigStoragePageSize = FLASH_PAGE_SIZE;

} // namespace port
