#pragma once

#include <cstdint>

namespace port {

// Bring the CPU to the firmware's target operating point. On RP2350 this sets
// the core voltage and system clock; on ESP32-S31 clock setup is handled by the
// bootloader and sdkconfig, so this only asserts the result.
void clocks_init();

// True while the board's user button is held.
//
// RP2350 has no dedicated user button: BOOTSEL is read by briefly floating the
// QSPI CSn line, which requires parking the other core (flash_safe_execute).
// That makes this call comparatively expensive, so keep it on the existing
// ~10 Hz poll cadence rather than sampling it freely. The
// ESP32-S31-Function-CoreBoard-1 has a real BOOT button and reads it cheaply.
bool boot_button_pressed();

// Register the calling core as a flash-safe victim, so a flash erase/program on
// the other core can park it. Called once from the audio worker at startup.
void worker_flash_safe_init();

// Force the board's switching regulator into PWM mode where the board has one
// under software control. Pico W / Pico 2 W expose an SMPS mode pin through the
// CYW43; left in PFM/burst mode the regulator whines audibly under this
// firmware's load (upstream #207). No-op on targets without such a pin.
void smps_force_pwm();

bool watchdog_caused_reboot();
void watchdog_enable(uint32_t timeout_ms);
void watchdog_update();

// Warm reset that re-runs the application from flash. Deliberately NOT a
// watchdog reset: on RP2350 a watchdog reset drops into the bootrom's BOOTSEL
// mode instead of restarting the app.
[[noreturn]] void reboot();

// Whether reboot_to_bootloader() can actually reach a flash mode on this
// target. True on RP2350, where the bootrom is one call away and BOOTSEL
// otherwise has to be held *during* power-up -- so the gesture genuinely saves
// an unplug/replug. False on ESP32-S31: download boot exists in silicon (there
// is even an EFUSE_DIS_FORCE_DOWNLOAD to switch it off) but is entered by
// strapping GPIO61 low at reset, and the register that forces it from software
// is not exposed for this target anywhere in ESP-IDF -- S2/S3 use
// RTC_CNTL_OPTION1_REG.FORCE_DOWNLOAD_BOOT, and the S31 has no rtc_cntl_reg.h
// at all. Guessing an address on preview silicon is how boards get wedged, so
// callers should offer their own fallback rather than call this blindly.
bool has_bootloader_reboot();

// Enter the ROM bootloader so the host sees a firmware-update device.
// Does not return. Only meaningful when has_bootloader_reboot() is true.
[[noreturn]] void reboot_to_bootloader();

// Bluetooth controller transport. On RP2350 this is the CYW43 driver over
// PIO-SPI and must be pumped from the main loop; on ESP32-S31 the controller is
// on-die and poll() is a no-op.
bool bt_transport_init();
void bt_transport_poll();

} // namespace port
