#pragma once

#include <cstddef>
#include <cstdint>

// Persistent storage for the single config record.
//
// The RP2350 build maps the last flash sector directly (XIP) and erases it in
// place; ESP-IDF cannot erase a partition that is currently mmap'd, so the
// ESP32-S31 backend keeps a RAM shadow and re-reads after each write. Callers
// must therefore treat the returned pointer as valid only until the next
// config_storage_write() — which is how the existing code already uses it.

#if defined(PORT_PLATFORM_PICO)
#include "pico/port_flash_impl.h"
#elif defined(PORT_PLATFORM_ESP32S31)
#include "esp32s31/port_flash_impl.h"
#endif

namespace port {

// kConfigStorageSize / kConfigStoragePageSize come from the impl header above
// as constants, so config.cpp can keep asserting its record fits at compile
// time rather than discovering it at runtime.

// Stable, readable view of the persisted bytes. Never null.
const void *config_storage_read();

// Pointer to the *previous* config location, for one-time migration, or nullptr
// on targets that never had one. RP2350 kept the config in the last flash
// sector until upstream #244 moved it clear of BTstack's link-key bank.
const void *config_storage_read_legacy();

// Erase the region and program `len` bytes of `data` at its start.
//
// Both backends guarantee the other core is not executing from flash for the
// duration: the Pico build parks core1 via flash_safe_execute (without which
// the audio core races the erase and buzzes), and the ESP32-S31 build relies on
// esp_partition_* taking the flash-op lock.
bool config_storage_write(const void *data, size_t len);

} // namespace port
