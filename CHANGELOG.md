# Changelog

All notable changes to this fork are documented here. See `awalol/DS5Dongle` for upstream history before v0.5.3.

## [0.5.4] — 2026-05-15

This release brings a security/correctness audit pass, defensive hardening, and a new optional Pico-OLED-1.3 status display add-on.

### Critical fixes

- **Audio stack overflow in `core1_entry`** (`src/audio.cpp`). `WDL_ResampleSample out_buf[200]` (800 bytes) was being passed to `resampler.ResampleOut(...)` with `nsamples_out = 480` — writing up to 480 stereo frames = 960 floats = 3840 bytes. `opus_encode_float` then read the same 960 floats. The buffer was overrun by ~3 KB on every audio frame. This is the very likely root cause of the previously documented "audio may experience slight stuttering" known issue. Audio is now clean on the same overclock.

### Security & correctness

- `set_feature_data`: variable-length array sized by host-controlled length replaced with a bounded fixed buffer; rejects `len < 4` (would have triggered CRC underflow).
- `tud_hid_set_report_cb` / `tud_hid_get_report_cb`: bounds-check before memcpy into stack / host buffers.
- `on_bt_data` / `l2cap_packet_handler`: length-check BT packets before indexing.
- UAC1 volume parsed as signed `int16` instead of `uint16` (was wrapping negative dB values to large positives).
- BT report sequence counter in `main.cpp` made consistent with `audio.cpp` (`& 0x0F`).
- `gap_inquiry_start(30)` was commented out in 3 pairing-failure handlers in upstream, leaving the device stuck after auth/connection failures. Uncommented — failed-pairing recovery is now automatic.
- Removed link-key UART `printf` (was leaking the BT bonding secret over the debug pin on every reconnect).
- File-scope counters and `inactive_time` made `static` (eliminates external-linkage collisions).
- Translated two Portuguese comments left from an upstream predecessor.
- Removed dead commented-out `main()` scaffolds.
- `set_feature_data` signature takes `const uint8_t*`; dropped a `const_cast` at the call site.
- Corrected misleading "Gamepad" CoD-filter comment.
- Renamed `opus_element opus_element{}` shadowing local to `elem`.

### Hardening

- 8-second hardware watchdog enabled in `main`, kicked once per main-loop iteration. Hangs now auto-recover.
- CRC helper functions (`fill_output_report_checksum`, `fill_feature_report_checksum`) early-return on `len < 4` (defense in depth).
- All 8 `hci_send_cmd` call sites wrapped in an `HCI_LOG_CMD` macro that logs non-zero returns. Silent HCI command drops were leaving the state machine stuck.
- `feature_data` map capped at 32 entries (small DoS hardening against a bonded peer flooding unique report IDs).

### New feature — optional Pico-OLED-1.3 status display

A Waveshare Pico-OLED-1.3 (128×64 SH1107) can be plugged on top of the Pico2W headers for a live status display. The firmware drives it automatically when present and gracefully no-ops when absent. See README for ASCII mockups and pinout.

Six screens, cycled with KEY0 on the add-on:

1. **Status** — connection state, paired DS5 BD address, battery percentage with bar (`+` charging / `*` complete / `!` error), live analog stick positions, D-pad indicator, face buttons (△ ◯ ✕ □), L1/R1, L2/R2 analog trigger fill bars
2. **Diagnostics** — uptime, HCI command-send error count, audio FIFO drops, Opus FIFO drops, BT state
3. **Trigger Test** — cycle 7 adaptive trigger effects with KEY1 (Off / Feedback / Weapon / Vibration / Bow / Gallop / Machine Gun); pull L2/R2 to feel
4. **Gyro Tilt** — live X/Y/Z accelerometer + 40×40 crosshair box tracking tilt
5. **Touchpad** — live 120×30 touchpad render with finger dots and finger count
6. **Lightbar Color Picker** — live tilt-driven RGB color preview on the DualSense's actual lightbar; 4 favorite slots saved via face buttons (△=0, ◯=1, ✕=2, □=3), recall by cycling with KEY1

KEY1 is context-sensitive:
- On Trigger Test: cycles trigger preset
- On Lightbar: cycles between LIVE preview and 4 favorite slots
- Anywhere else: sends a 250 ms test-rumble burst (validates firmware → controller output path without a host)

Boot splash for 1.5 seconds on power-on before the status screen takes over.

Implementation notes:
- In-tree minimal SH1107 driver + 5×7 ASCII font, dependency-free (~600 LOC `src/oled.cpp` + 95-char × 5-byte font table)
- SPI1 at 10 MHz on the standard Waveshare pin layout (DC=GP8, CS=GP9, CLK=GP10, MOSI=GP11, RST=GP12, KEY0=GP15, KEY1=GP17)
- No conflicts with UART, USB, or CYW43 internal SPI
- 10 Hz display refresh; ~1 % CPU
- Lightbar updates sent at the same 10 Hz rate while on the Lightbar screen (~780 B/s of BT traffic, negligible)

### Build / dependencies

- Pico SDK **2.2.0**
- TinyUSB submodule must be **0.20.0 or newer**. The SDK's bundled 0.18.0 ships a 3-argument `TUD_AUDIO_EP_SIZE` macro that doesn't compile against this project's `tusb_config.h`. Inside `<pico-sdk>/lib/tinyusb`, `git checkout 0.20.0` resolves it.
- ARM toolchain: `gcc-arm-none-eabi 14.2.1+`
- New CMake link library: `hardware_spi` (for the OLED driver)
- New project source: `src/oled.cpp`, `src/oled.h`, `src/oled_font.h`

### Documentation

- `README.EN.md` gained a comprehensive **Hardware** section with vendor links and approximate prices so users can replicate the setup.
- **Known Issues** updated: audio-stutter listed as fixed in this branch; overclock reframed as load-bearing for the CYW43 BT clock (dropping voltage alone or `sys_clk` alone both break BT pairing — verified empirically).
- **OLED Display Add-on** section restructured with ASCII mockups, contextual KEY1 behavior table, and pinout.

### Project metadata

- Version bumped 0.5.3 → 0.5.4 (`CMakeLists.txt`).

---

## [0.5.3] and earlier

See `awalol/DS5Dongle` upstream for original history. The English translation commit is the immediate parent of this changelog's 0.5.4.
