# Repository guidance

## Read the implementation

DS5Dongle is RP2040/RP2350 firmware for a USB to Bluetooth DualSense bridge. It
does not contain the DS5 Bridge Windows companion, WinUSB transport, or installer.
Start with `README.md` and `.github/workflows/build-firmware.yml`; use source
code as the authority when older documentation differs.

`src/main.cpp` owns startup and the Core 0 poll loop. `src/bt.cpp` handles the
controller connection, `src/usb_descriptors.cpp` and `src/usb.cpp` define the
host facing USB device, and `src/audio.cpp` runs audio work on Core 1. HID
configuration is implemented in `src/cmd.cpp`, `src/config.h`, and
`src/config.cpp`; `tools/config_tool.py` is the corresponding host tool.

## Build and verify

For reproducible firmware builds, follow `.github/workflows/build-firmware.yml`:
Pico SDK `2.3.0`, its TinyUSB checkout at
`2d56dc533e45e4e91b15e93fdab5e22e964f328d`, Arm GNU Toolchain
`15.2.Rel1`, CMake, and Ninja. Initialize this repository's `lib/WDL` and
`lib/opus` submodules with `git submodule update --init --recursive`. Verify
the SDK and TinyUSB Git revisions and compiler version rather than assuming a
local SDK installation matches CI. The convenience scripts may use different
dependency revisions; use the workflow pins for release evidence.

Configure each Release variant in its own build directory, with
`-DPICO_SDK_PATH=<sdk-path>` and the intended `-DVERSION=<version>`. The
default board is Pico 2 W. Use `-DPICO_W_BUILD=ON` for Pico W (speaker
processing disabled), or `-DWAVESHARE_RP2350B_PLUS_W_BUILD=ON` for Waveshare
RP2350B-Plus-W. These board options are mutually exclusive. CI's debug
variant uses `-DENABLE_SERIAL=ON -DENABLE_VERBOSE=ON -DWAKE_DEBUG=ON`; it is
built with `CMAKE_BUILD_TYPE=Release` and is a separate UF2. Build with
`cmake --build <build-dir> --target ds5-bridge` and inspect that directory's
`ds5-bridge.uf2`. Do not substitute a UF2 from another board or configuration.

The Pico 2 W 225/300 MHz experimental profiles are opt-in, default off, and
unsupported. Keep their CPU, voltage, CYW43, and flash settings coupled; do
not restore the retired 375 MHz profile or include experimental UF2s in a
standard release. See [Experimental Pico 2 W clocks](docs/pico2w-experimental-clocks.md)
for the exact settings, validation, electrical limits, and failure history.

Run all native CTests in `tests/` for firmware policy changes, and the Python
unittests in `tests/test_config_tool.py` for configuration protocol or CLI
changes. Record the source and dependency revisions, build options, board,
artifact SHA-256, and complete check results. Builds, static inspection, host
tests, and `picotool` output do not qualify device boot, pairing, audio,
stability, actual host HID receive rate, or game behavior.

## Preserve firmware contracts

The configuration protocol uses DualSense gamepad HID feature reports:
`0xF6` writes settings or commands, `0xF7` reads the 22-byte `Config_body`
and an optional five-byte battery-feedback tail, `0xF8` reads the firmware
version, and `0xF9` reads RSSI plus optional audio flags. Keep `src/cmd.cpp`,
`src/config.h`, `src/config.cpp`, the report descriptors, `tools/config_tool.py`,
and the paired Web configurator aligned whenever report IDs, lengths, field
order, validation, or schema version change. Keep Config v5/22 bytes and the
independently versioned battery extension compatible: old padded `0xF6/0x01`
writes must not clear the extension; `0xF6/0x05` updates its RAM flag, and
`0xF6/0x02` saves both records. Keep `0xF6/0x04` reserved; the current
command handler does not implement it, so do not rely on the legacy
`tools/reboot_bootsel.py` helper without device verification. Flash-backed
settings and BTstack pairing storage are separate. Wake and keyboard
behavior is compiled into the base firmware but enabled through runtime
configuration; descriptor or USB re-enumeration changes need device checks.

Runtime `polling_rate_mode` values 0 (250 Hz), 1 (500 Hz), and 2 (dirty
Real-time) retain their existing behavior. Mode 3 uses a fixed 1 ms USB HID
cadence (endpoint interval) and queues the latest complete 63-byte
Bluetooth-derived input state whenever the endpoint is ready. If no new
Bluetooth report arrives, it repeats the same state, including sensor values
and controller timestamp; do not fabricate fresh sensor samples or timestamps.
When Web/CLI changes the mode, use the Web reconnect action or
`python tools/config_tool.py reconnect` (both send `0xF6` function `0x03`),
or instruct a physical replug so the host reads the new HID endpoint
interval. Do not infer 1,000 actual host receives per second or improved gyro
jitter from builds or endpoint metadata.

Battery feedback consumes only genuine, complete Bluetooth battery reports
from the current connection. Preserve raw-level confirmation by later real
reports, immediate cancellation on charging or disable, and connected
handback to the latest known host Player/RGB intent. The Pico onboard LED remains
independent. Update the native battery tests when changing these rules.

Core 0 services CYW43, TinyUSB, and the audio queues; it services the watchdog
when enabled (the serial debug build disables it). Core 1
processes audio. Preserve `flash_safe_execute_core_init()` on Core 1 and
`PICO_FLASH_ASSUME_CORE1_SAFE=0`: `config_save()` and BOOTSEL polling must park
Core 1 before QSPI flash access. Audio timing also depends on the RAM
relocations in `CMakeLists.txt` and `cmake/relocate_to_ram.cmake` for Opus, WDL,
and selected Bluetooth/USB hot paths. Review linked SRAM use, stack bounds,
queue behavior, and actual audio output when changing those paths.

## Package releases

The release workflow uploads three assets: the versioned standard UF2,
`config_tool.py`, and `other board.zip` containing the Pico W and Waveshare
UF2s. It excludes the debug and experimental UF2s. Keep experimental outputs
separate with provenance and an unqualified label; an upload is not device
qualification.
