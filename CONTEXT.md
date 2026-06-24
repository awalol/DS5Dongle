# Project Context — Pico 2W DualSense Bridge (DS5Dongle) — **v0.7.2-hotfix + Wake-on-LAN**

> Status: **functional and validated on real hardware** (target PC with an RTX 5090).
> For build and flash instructions, see [BUILD.md](BUILD.md). For the next round of
> improvements, see [IMPROVEMENTS.md](IMPROVEMENTS.md).

---

## 1. What this project is

Firmware for a **Raspberry Pi Pico 2W** (RP2350 + CYW43 wireless chip) that turns it
into a **wireless USB dongle** for Sony **DualSense (DS5)** and **DualSense Edge (DSE)**
controllers. It acts as a bidirectional bridge:

```
   DualSense  ──(Bluetooth Classic, L2CAP/HID)──►  Pico 2W  ──(USB HID + Audio)──►  PC
```

- **Toward the controller:** a Bluetooth Classic host (BTstack) — inquiry, pairing,
  opening the L2CAP HID channels (Control + Interrupt).
- **Toward the PC:** it emulates a real DualSense (Sony VID/PID) with an HID interface
  plus USB Audio (UAC1), including haptic rumble and speaker/jack audio.
- **Dual-core:** core0 = main loop (BT, USB, haptics, WoL); core1 = Opus audio codec.

---

## 2. The added feature: Wake-on-LAN (`ENABLE_WOL`, ON by default)

**Goal achieved (a console-like experience):** with the PC **powered off (S5)** but the
Pico still powered (USB standby or a powered hub/extender), a **single PS press** on the
controller connects it to the Pico, which then sends a **WoL magic packet** over WiFi.
The PC powers on and the controller **stays connected throughout the entire boot** (no
need for a second PS press). Once the PC is up, WiFi is brought down and the controller
works as usual.

### Files added (`src/`)
- [wol.cpp](src/wol.cpp) / [wol.h](src/wol.h) — non-blocking state machine
  (`Idle → Observe → Connecting → Backoff → Sending → Cleanup`).
- [lwipopts.h](src/lwipopts.h) — lwIP configuration (poll, UDP + DHCP + broadcast).
- `secrets.h` — local credentials (SSID, password, **PC MAC**, port).
  **Gitignored**; template at [secrets.h.example](src/secrets.h.example).

### Hooks into existing files
- [CMakeLists.txt](CMakeLists.txt) — `ENABLE_WOL` option; when ON: `CYW43_LWIP=1`,
  links `pico_cyw43_arch_lwip_poll`, compiles `wol.cpp`, **and does NOT relocate Opus to
  RAM** (see §3.1).
- [main.cpp](src/main.cpp) — `wol_init()` after `state_init()`; `wol_tick()` at the end
  of the loop.
- [bt.cpp](src/bt.cpp) — `wol_request()` when the L2CAP HID Interrupt channel opens.
- [wake.cpp](src/wake.cpp) / [wake.h](src/wake.h) — new `wake_suppress_poweroff()`
  function (see §3.3).

---

## 3. The THREE problems solved (exact cause + fix)

### 3.1 `*** PANIC *** Out of memory` → hang during pairing

- **Symptom:** with the WoL build (and even the stock+serial build), the Pico would find
  the controller but **hang during SSP**; it never completed the connection. The official
  precompiled binary **did** work.
- **Cause:** the firmware relocates **~280 KB of Opus code to RAM** (`.time_critical`,
  for audio performance). This leaves the heap very tight. Adding lwIP (+~18 KB) for WoL
  caused a `malloc` during BT initialization to fail → `panic("Out of memory")`. (BTstack
  uses static pools; the OOM came from the heap shared between Opus and lwIP.)
- **Fix:** in [CMakeLists.txt](CMakeLists.txt), **when `ENABLE_WOL` is active, `libopus`
  is NOT relocated to RAM** (it stays in flash/XIP). This frees ~280 KB → from ~83 KB
  free to **~362 KB free**. The stock build (no WoL) keeps Opus in RAM as in the original.
- ✅ **Trade-off RESOLVED (performance release, §3.4):** there is no longer a choice
  between WoL and audio. Only the **CELT encode path** of Opus (~87 KB) is relocated to
  RAM, not the full 241 KB → perfect audio **and** ~276 KB of free heap.
- **Build note:** the `objcopy`/`ar` step that renames the sections modifies `libopus.a`
  *in place*, so a **clean build** (delete `build*/`) is required for the change to take
  effect.

### 3.4 Performance release — perfect audio with RAM headroom *(2026-06-24)*

- **Key insight:** **both directions run CELT-only** — the speaker encoder uses
  `OPUS_APPLICATION_RESTRICTED_LOWDELAY`, and the DualSense mic streams CELT-only 48 kHz
  fullband 10 ms (SILK cannot represent 48 kHz fullband). The SILK code **never** runs on
  the audio path. So only the CELT encode+decode subset (21 TUs, ~104 KB of `.text`) is
  relocated to RAM instead of all of `libopus` (~241 KB). This eliminates the per-frame
  XIP misses on core1 for both **encode** (speaker/haptics) and **decode** (mic) → perfect
  audio in both directions, leaving ~260 KB of free heap — **>100 KB** above the level that
  triggered the OOM (§3.1).
- **Mechanism:** [cmake/relocate_archive_members.cmake](cmake/relocate_archive_members.cmake)
  performs per-member surgery on `libopus.a` (`ar x` → `objcopy --rename-section
  .text=.time_critical.opus_text` → `ar r` → `ar s`). In [CMakeLists.txt](CMakeLists.txt),
  the `ENABLE_WOL` ON branch performs the **selective** relocation (`OPUS_RAM_MEMBERS`);
  OFF keeps relocating the whole archive (original behavior). Verified in the map:
  `celt_encode_with_ec`, `celt_decode_with_ec`, `opus_decode`, `ec_dec_*`, etc. at
  `0x2001xxxx` (RAM); `silk_*` at `0x10xxxxxx` (flash).
- **Other wins in this release (zero risk, build-verified):**
  - HCI transport bridge btstack↔cyw43 (`hci_transport_data_source_process`,
    `..._send_packet`, `cyw43_bluetooth_hci_process`) relocated to RAM → less XIP latency
    on the per-packet BT path that feeds the audio/haptics reports.
  - btstack's `ENABLE_LOG_INFO` removed (verbose internal logging; less flash/CPU on the
    BT event path). `ENABLE_PRINTF_HEXDUMP` is kept: the Pico SDK compiles
    `hci_dump_embedded_stdout.c`, which requires it for an `#error`, but it is
    *gc-eliminated* from the final binary (hci_dump is never initialized).
  - `<iostream>`/`<iomanip>` + the dead `print_hex()` removed from `utils.h`.
  - CMake guard: `FATAL_ERROR` if anyone enables IPO/`-flto` (it would silently break the
    entire `.time_critical` relocation, which runs at PRE_LINK).
- **Micro-opts discarded after verification:** `/32768.0f`→reciprocal is already done by
  the compiler (`vdiv=0`); other hoists were on non-critical paths or pessimized the
  short-circuit → the BT/audio code is left untouched.
- **Mic decode (2026-06-24, 2nd iteration):** added the CELT-decode set
  (`celt_decoder` + `opus_decoder` + `entdec`, +17 KB) to `OPUS_RAM_MEMBERS` → perfect mic
  as well. `.data` 150 → 167 KB; heap ~276 → **~260 KB** (safe, >250 KB). The mic codec is
  confirmed CELT by analysis and validated by ear.
- **Pending (needs hardware measurement, see [IMPROVEMENTS.md](IMPROVEMENTS.md)):**
  reclaiming the core1 stack (32 KB), trimming lwIP (~9 KB), and optionally Opus's
  `.rodata` tables (+20 KB).

### 3.2 WoL only fired the first time (suspended-bus gating)

- **Symptom:** the 1st time (PC never powered on) WoL was sent. The 2nd time (PC had been
  on and was then powered off) it **was not sent**.
- **Cause:** the gating looked only at `tud_mounted()`. When the PC powers off but the USB
  port keeps supplying current, the bus is **suspended** (not disconnected) and
  `tud_mounted()` stays `true` → WoL assumed the PC was on and aborted.
- **Fix:** in [wol.cpp](src/wol.cpp), `Observe` state, the abort condition is now
  **`tud_mounted() && !tud_suspended()`** (host *mounted AND active*). If suspended (PC at
  S5 with standby) or unmounted → WoL fires. The debounce was also raised to **90 s** so
  that reconnections during boot do not re-trigger WoL.

### 3.3 Connection dropped during boot → a 2nd PS was needed

- **Symptom:** WoL woke the PC, but **during boot** the controller powered off and a second
  PS press was required. It also happened with stable power (extender) → it was neither
  power nor enumeration.
- **Key diagnostic:** logs captured over WiFi (UDP) + user observation (the USB keyboard
  LED and the Pico LED blinked at the BIOS→Windows handoff).
- **Exact cause:** [wake.cpp](src/wake.cpp) has a battery-saving feature that **powers off
  the controller** (`bt_power_off_controller()`) if USB stays **suspended for more than 3 s**
  (`WAKE_POWEROFF_DEBOUNCE_US`), interpreting it as "PC asleep/off." On a post-WoL boot, USB
  is suspended for many seconds while the PC starts → the firmware powered off the controller
  mid-boot.
- **Fix (universal, does not break S3 suspend):** new
  **`wake_suppress_poweroff(duration_us)`** function in [wake.cpp](src/wake.cpp). WoL calls
  it (from `wol_request()`, [wol.cpp](src/wol.cpp)) when the controller connects, suppressing
  the automatic power-off for **180 s** (covers the boot). If the PC really doesn't boot,
  the power-off re-enables after the timeout (battery saving intact). The S3 suspend behavior
  (power off after 3 s + wake on PS) is **unchanged**.

---

## 4. Build options (CMake)

| Option | Default | Effect |
|--------|---------|--------|
| `ENABLE_WOL` | **ON** | Wake-on-LAN; lwIP; **Opus CELT-encode in RAM** (selective, §3.4) |
| `ENABLE_SERIAL` | OFF | `printf` over USB CDC (changes USB enumeration; no watchdog) |
| `ENABLE_VERBOSE` | OFF | Detailed BTstack logs |
| `ENABLE_BATT_LED` | ON | Low-battery LED |
| `WOL_FORCE_TEST` | OFF | **DEBUG:** forces WoL on every connection (ignores PC gating) |
| `WOL_UDP_LOG` | OFF | **DEBUG:** redirects `printf` to UDP broadcast:9999 and keeps WiFi up |
| `PICO_W_BUILD` | OFF | Pico W (RP2040) |
| `WAKE_DEBUG` | OFF | Trace of the wake FSM |

> The two DEBUG `WOL_*` options are diagnostic tools; in production they are **OFF**.
> `WOL_UDP_LOG` keeps WiFi active (stresses BT coexistence) — use it only to capture logs
> with the target PC powered off.

---

## 5. Validated status (real hardware)

- ✅ Clean pairing (OOM resolved).
- ✅ WoL reliably wakes the target PC (RTX 5090), including repeatedly (suspended-bus
  gating resolved).
- ✅ The controller stays connected during boot → **a single PS** (automatic boot-time
  power-off resolved).
- ✅ With the PC on, everything works normally and WoL aborts correctly.
- 🔬 **Performance release (§3.4, 2026-06-24): hardware validation pending.** Build verified
  in the map (CELT-encode in RAM, SILK in flash, ~276 KB free heap) and flashed. It still
  needs to pass the hardware test matrix (see IMPROVEMENTS.md): speaker/mic/haptics audio,
  pair/reconnect, BOOTSEL, wake-on-PS, cold WoL, DSE, config-save, and ≥10 pair+WoL cycles
  without a PANIC.

### Validated hardware / setup
- Pico 2W with the production firmware (`build/ds5-bridge.uf2`, `ENABLE_WOL=ON`).
- Recommended stable power: a USB hub/extender with external power, or a PC USB port with
  "always-on USB" (ErP off) to supply standby current.

---

## 6. Change log (work log)

### 2026-06-23 — WoL port and stabilization on real hardware
- Cloned the `v0.7.2-hotfix` tag; ported WoL from the work branch on top of 0.6.0.
- Built with Pico SDK 2.2.0 + TinyUSB **0.20.0** + ARM GCC 14.2.Rel1 (see BUILD.md).
- **Fix 3.1:** OOM resolved by keeping Opus out of RAM when `ENABLE_WOL` is set.
- **Fix 3.2:** WoL gating is now `tud_mounted() && !tud_suspended()`; debounce 90 s.
- **Fix 3.3:** `wake_suppress_poweroff()` keeps the controller from powering off during PC
  boot.
- Added the `WOL_FORCE_TEST` and `WOL_UDP_LOG` diagnostic tools.
- Validated on real hardware: single-PS experience achieved.

### 2026-06-24 — Performance release (perfect audio + RAM headroom)
- Thorough multi-dimensional analysis (7 dimensions, adversarial verification) → a phased
  plan.
- **Selective CELT-encode relocation** of Opus to RAM (§3.4): resolves the audio trade-off
  from fix 3.1. `.data` 64 KB → 150 KB; free heap ~362 KB → **~276 KB**; perfect audio.
- HCI bridge btstack↔cyw43 to RAM; `ENABLE_LOG_INFO` removed; `<iostream>`/`print_hex`
  removed; anti-LTO guard in CMake.
- Clean build verified (map: CELT in RAM, SILK in flash) and flashed. Hardware validation
  pending. Upcoming improvements (stack measurement, lwIP, decode/rodata) in IMPROVEMENTS.md.

> Background (original analysis and review) is in the 0.6.0 folder.

---

## 7. ⚠️ Security / git

- `src/secrets.h` (SSID, **WiFi password**, MAC) is **gitignored** and **must never be
  pushed**. Public template: `src/secrets.h.example`.
- The `origin` remote points to the upstream `awalol/DS5Dongle`. To push your changes,
  **create your own repository** and change the remote; do not push to upstream.