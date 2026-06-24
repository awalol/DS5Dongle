# Pico2W DualSense 5 Bridge — Wake-on-LAN fork

[中文](./README.CN.md)

> Turn a Raspberry Pi Pico2W (or other compatible board) into a wireless adapter for the DualSense (DS5) controller.

This is a fork of [**awalol/DS5Dongle**](https://github.com/awalol/DS5Dongle). It keeps
everything the upstream firmware does and adds **Wake-on-LAN**: with the PC fully powered
off, one press of the PS button wakes it over the network and the controller stays
connected through the whole boot. The change is engineered so it does **not** cost the
upstream's RAM-resident, glitch-free audio — see [Performance](#performance).

### What this fork adds

- 🌐 **Wake-on-LAN over Wi-Fi** — wake a fully powered-off PC (S5) with a single PS press.
  Complements the upstream's [Wake-on-PS](#wake-on-ps-optional), which wakes a *sleeping*
  PC (S3). See [Wake-on-LAN](#wake-on-lan).
- 🧠 **Selective Opus relocation** — keeps the audio codec in RAM (so audio stays
  glitch-free at the stock 150 MHz) *while* leaving room for the Wi-Fi stack, instead of
  running out of memory. Details under [Performance](#performance).
- 🩹 A few smaller latency/footprint cleanups, also in [Performance](#performance).

Everything below applies to this fork. Upstream-only links (web config, releases) are
noted where relevant.

## Overview

This project enables the Raspberry Pi Pico2W (or other compatible board, e.g. the Waveshare RP2350B-Plus-W) to function as a Bluetooth bridge for the DualSense controller, allowing wireless connectivity with enhanced haptics support.

## Features

- 🎮 Full DualSense connectivity via Pico2W (or other compatible board)
- 🔊 Supports HD haptics (advanced vibration feedback)
- 🎧 Headset audio output — controller speaker and 3.5 mm jack
- 🎤 Headset microphone input — the controller mic is exposed as a USB audio input device
- 📡 Wireless Bluetooth bridging
- 🌐 **Wake-on-LAN** — wake a powered-off PC from the controller (this fork)
- 🔘 BOOTSEL-button controller management — pair, reboot, enter BOOTSEL for flashing, or forget all pairings without unplugging
- ⚡ Runs at the stock 150 MHz clock — no overclock required

## Wake-on-LAN

With the PC **fully powered off (S5)** but the Pico still powered (from an always-on USB
port or a powered hub/extender), a **single press of the PS button** does the whole thing:
the controller connects to the dongle, the dongle joins Wi-Fi and sends a Wake-on-LAN
**magic packet** to the PC, the PC powers on, and the controller **stays connected
through the entire boot** — no second press. Once the host is up, Wi-Fi is brought back
down and the dongle behaves exactly like the normal bridge.

This is different from, and complementary to, the upstream [Wake-on-PS](#wake-on-ps-optional)
feature:

| | Wake-on-PS (upstream) | Wake-on-LAN (this fork) |
|---|---|---|
| Wakes from | **S3 sleep** | **S5 / fully off** |
| Mechanism | USB HID remote wakeup | Wi-Fi magic packet to the PC's MAC |
| Needs | USB bus kept alive while asleep | Pico powered + same LAN as the PC |

### Requirements

- The Pico must stay powered while the PC is off — use an **always-on USB port** (BIOS
  "ErP off" / charging port) or a **powered USB hub/extender**.
- The PC's network adapter must have **Wake-on-LAN enabled** (in BIOS/UEFI and in the OS
  NIC driver). WoL targets the **MAC address**, and the packet is an L2 broadcast, so the
  Pico's Wi-Fi must be on the **same subnet** as the PC.

### Setup

WoL needs your Wi-Fi credentials and the target PC's MAC address. These live in
`src/secrets.h`, which is **git-ignored** and must never be committed (it contains your
Wi-Fi password and is baked into the Pico's flash). A template is provided:

```sh
cp src/secrets.h.example src/secrets.h   # then edit src/secrets.h
```

```c
#define WIFI_SSID       "YOUR_SSID"
#define WIFI_PASS       "YOUR_WIFI_PASSWORD"
#define WOL_TARGET_MAC  "AA:BB:CC:DD:EE:FF"   // MAC of the PC's WoL-armed adapter
#define WOL_PORT        9
```

On Windows, find the MAC with `ipconfig /all` → "Physical Address" of the adapter that
has WoL armed (usually the Ethernet one). Then build normally (WoL is on by default):

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_SDK_PATH=<sdk>
cmake --build build --target ds5-bridge
```

To build without WoL, configure with `-DENABLE_WOL=OFF`.

> **Pre-built firmware does not carry your credentials.** The `.uf2` files from
> [Releases](../../releases) / CI are built **without** `secrets.h`, so they compile with
> placeholder Wi-Fi/MAC values and Wake-on-LAN will not wake anything. To use WoL you must
> **build from source with your own `src/secrets.h`**. Every other feature works on the
> pre-built firmware as usual.

## Getting Started

### Get the firmware

You have two options:

- **Download a pre-built `.uf2`** — grab the newest
  [Releases](../../releases) build (`ds5-bridge-<version>.uf2`; other board
  builds are bundled in `other board.zip`; `config_tool.py` is attached there
  too). No tools needed. (Wake-on-LAN is inert on these builds — see the note above.)
- **Build it yourself** — see [Build Instructions](#build-instructions)
  below (Windows users get a one-command script). Required for Wake-on-LAN.

### Flashing Firmware

1. Hold the BOOTSEL button on the Pico2W
2. Connect the Pico2W to your computer via USB
3. The device will mount as a USB storage device
4. Drag and drop the .uf2 firmware file onto the device

> The firmware also supports a **reboot-to-BOOTSEL** command: the **Reboot to Bootloader** button in the
> [web config](#configuration) reboots the dongle into BOOTSEL mode without holding the physical button.

### Pairing the Controller

1. Put the DualSense controller into Bluetooth pairing mode
2. Wait for the Pico2W to detect and connect
3. Once connected, the device will appear on the host system

***You may need to replug the Pico when the controller is in pairing mode.***

### BOOTSEL button: switch, reboot, or clear controllers

While the firmware is running, the Pico's **BOOTSEL button** doubles as a
controller and reset control — no unplugging or re-flashing needed:

- **Short press (click):**
  - If a controller is connected, the current one is disconnected (its pairing is
    kept, so it can reconnect later). Use this to free the dongle for a different
    already-paired controller.
  - If nothing is connected, a 30-second scan starts to pair a new controller.
    Put the DualSense into pairing mode (hold **PS + Create/Share** until the
    light bar flashes) while the scan runs.
- **Double click:** **Reboot the Pico** — a normal firmware restart: re-enters
  pairing inquiry, drops the current connection, and recovers from a transient
  glitch. (Clicks register after a brief pause, to allow for a second/third click.)
- **Triple click:** **Reboot into BOOTSEL** — the dongle re-enumerates as a USB
  mass-storage drive so you can drag on a new `.uf2`, without holding BOOTSEL while
  plugging in.
- **Long press (~1.5 s):** Disconnect and **forget every paired controller** — all
  stored pairings are deleted and blacklisted so they won't silently auto-reconnect,
  even across a power cycle. The onboard LED flashes six times to confirm. To use a
  forgotten controller again, put it back into **PS + Create/Share** pairing mode.

> Triple click is a software path into the bootloader; you can also still enter it
> the hardware way by holding BOOTSEL **while plugging in** the Pico (see
> [Flashing Firmware](#flashing-firmware) above). All of these act on
> click / double / triple / long-press **while the firmware is already running**.

## Configuration

You can modify the Pico settings via the web config (hosted by upstream; it works with
this fork's firmware over the same HID protocol). Wake-on-LAN itself has no web-config
toggle — it is configured at build time via `src/secrets.h` (see [Setup](#setup)).

- For release: https://ds5.awalol.eu.org
- For development: https://ds5-dev.awalol.eu.org

## Community Fork

### Audio Auto Haptics fork [loteran/DS5Dongle](https://github.com/loteran/DS5Dongle)

> Adds real-time haptic feedback generated from game audio.
> The Pico listens to the sound stream and converts bass and impact sounds into DualSense rumble — no game-side haptic
> support needed.

### DS5_Bridge [SundayMoments/DS5_Bridge](https://github.com/SundayMoments/DS5_Bridge)

> More customization features, such as adjusting audio, haptics, trigger strength, lighting, button remapping, and
> shortcuts.

### OLED Edition [MarcelineVPQ/DS5Dongle-OLED-Edition](https://github.com/MarcelineVPQ/DS5Dongle-OLED-Edition)

> OLED Edition is a fork of awalol/DS5Dongle (upstream) that adds an optional Pico-OLED-1.3 128×64 display add-on with
> 11 screens (status, 4-slot multi-controller pairing, lightbar color picker with favorites and effect presets, trigger
> test, gyro tilt, touchpad, diagnostics, CPU/clock, BT signal strength, audio VU meters, and a persistent settings menu),
> plus a DS5 button-combo soft-reboot.

### [zurce/DS5Dongle-OLED](https://github.com/zurce/DS5Dongle-OLED)

## Performance

Upstream already runs the time-critical audio path — libopus encode/decode, the
resampler, and the Bluetooth/USB hot path — from **RAM** instead of flash, which removes
flash-fetch (XIP cache-miss) stalls from the audio loop and lets the **full audio path
run at the stock 150 MHz clock with no overclock and no core-voltage bump** (earlier
releases needed 320 MHz @ 1.2 V). This fork preserves that.

The challenge this fork solves is keeping that property **while adding Wake-on-LAN**.
Wake-on-LAN brings in the lwIP network stack, which shares the heap with the codec.
Relocating the *whole* `libopus` archive to RAM (the stock approach) plus lwIP exhausts
the allocator during Bluetooth pairing (`*** PANIC *** Out of memory`); the naive
workaround — pushing Opus back to flash — brings the audio stutter straight back.

**The fix: relocate only the CELT codec, not all of Opus.** Both audio directions run
Opus in `OPUS_APPLICATION_RESTRICTED_LOWDELAY`, which is **CELT-only** — the speaker/haptics
encoder, and the controller-microphone decoder (CELT-only, 48 kHz, 10 ms frames). The SILK
code is never executed on the audio path, so it can stay in flash. This fork relocates
just the 21 CELT translation units (encode + decode, ≈104 KB of `.text`) to RAM via
per-member archive surgery (`cmake/relocate_archive_members.cmake`), instead of the full
≈241 KB library.

Result, measured on the RP2350 (520 KB SRAM), `ENABLE_WOL=ON` at 150 MHz:

| | Whole-archive Opus in RAM | This fork (CELT-only) |
|---|---|---|
| Opus `.text` resident in RAM | ≈241 KB | **≈104 KB** |
| Free heap | ≈121 KB → **OOM during pairing** | **≈260 KB** |
| Audio (speaker / haptics / mic) | glitch-free | glitch-free |

≈260 KB of free heap is more than **2×** the level at which pairing previously ran out of
memory, so Wi-Fi/lwIP coexists comfortably with the BT stack and the codec.

Additional changes in this fork:

- **Bluetooth HCI transport bridge moved to RAM** — the per-packet btstack↔CYW43 bridge
  (RX drain, TX, and the HCI read pump) ran from flash; relocating it removes XIP-miss
  latency from the per-packet path that carries the audio/haptics reports.
- **btstack verbose logging disabled** (`ENABLE_LOG_INFO` dropped) — removes per-event log
  strings and formatting from the release; error logging is kept.
- **Build guard** — IPO/`-flto` is rejected at configure time, because it would silently
  defer code generation past the section-rename step and leave every relocated hot path in
  flash.
- **Pico W (RP2040)** — the Opus relocation is skipped there: that board has only 264 KB of
  RAM and audio is disabled on it, so Opus stays in flash.

The net effect on day-to-day use is that controller **speaker, 3.5 mm output, haptics, and
microphone are clean** at the stock clock, with Wake-on-LAN available and no memory
pressure.

## Known Issues

_None currently tracked for this fork. Audio runs from RAM at the stock clock (see
[Performance](#performance))._

## Build Instructions

> For Wake-on-LAN, create `src/secrets.h` first (see [Setup](#setup)). Without it the
> firmware still builds — Wake-on-LAN just stays inert (placeholder credentials).

### Windows 11 (one command, no WSL)

You don't even need to clone this repo. Download just
[`tools/build-windows.ps1`](tools/build-windows.ps1) to any folder and run
it in **PowerShell**:

```powershell
powershell -ExecutionPolicy Bypass -File .\build-windows.ps1
```

(If you already have a checkout, run `tools\build-windows.ps1` from the
repo root instead — it detects and uses your local checkout.)

The script installs every prerequisite (CMake, Ninja, Python, Git and the
ARM GNU toolchain — via `winget`, falling back to portable downloads if
`winget` is unavailable), clones the project (if not run from a checkout)
plus the pinned Pico SDK + TinyUSB into `%USERPROFILE%\.ds5-build`, builds
the firmware, and drops `ds5-bridge.uf2` next to the script and on your
Desktop. It is safe to re-run; already-installed tools are skipped.

Build a fork or a specific ref with `-Repo <url>` / `-Ref <branch|tag>`.

Build a variant with `-Variant debug`.

### Other platforms

To build from source manually:

1. Install the Pico SDK 2.2.0 and switch its TinyUSB submodule to tag 0.20.0
   i.e. ***Update TinyUSB in the Pico SDK to the latest version***
2. Initialise this repo's submodules: `git submodule update --init --recursive`
3. (Optional, for Wake-on-LAN) `cp src/secrets.h.example src/secrets.h` and edit it
4. Configure and build with the standard Pico SDK toolchain:
   `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_SDK_PATH=<sdk>`
   then `cmake --build build --target ds5-bridge`

> A **clean build is required** if you change `ENABLE_WOL` or the Opus member list: the
> relocation step rewrites `libopus.a` in place, so delete the build directory before
> reconfiguring.

On macOS, `tools/build-macos.sh` can prepare a repo-local Pico SDK checkout, prompt to install missing Homebrew build
tools, initialize submodules, pin TinyUSB, and build the firmware:

```sh
tools/build-macos.sh
```

Use `tools/build-macos.sh --clean` to rebuild from scratch, or
`--sdk-dir <path>` to use an existing SDK checkout. When using `--sdk-dir`, the script asks before checking that SDK out
to the required Pico SDK and TinyUSB versions. If Homebrew's `arm-none-eabi-gcc` formula is installed without standard C
headers, the script asks to install the complete `gcc-arm-embedded` cask and points CMake at that toolchain.

## Notes

The Pico device will only be visible to the system after the controller is connected

Some behaviors depend on reconnection cycles to take effect

### Microphone

The controller microphone is exposed as a USB audio input — "Headset Microphone"
on Windows. After selecting it as your recording device, raise its input/capture
level in your OS: Windows in particular often defaults it to 0 (or very low),
which makes the mic seem dead even though it is working.

### Low-battery LED indicator

When the connected DualSense reports its battery at or below 10% (and it is not charging), the Pico onboard LED switches
from solid-on to a 1 Hz blink so you can see the warning at a glance. The LED returns to solid-on as soon as the
controller is plugged in or its reported level rises again. The blink also fires when `disable_pico_led` is set — the
warning is treated as critical and overrides the LED-off preference; the LED returns to its disabled (off) state once
the battery recovers or the controller starts charging.

To opt out at build time, configure with `-DENABLE_BATT_LED=OFF`. Default is ON.

### Pico W Version

Pico W only has haptics support, no speaker. You can enable Pico W firmware compilation with `-DPICO_W_BUILD=ON`, or
download precompiled firmware from GitHub Actions. (On the Pico W the Opus codec stays in flash — see
[Performance](#performance).)

### Waveshare RP2350B-Plus-W

The [Waveshare RP2350B-Plus-W](https://www.waveshare.com/wiki/RP2350B-Plus-W) is an RP2350B-based board with the RM2 wireless module (same CYW43 silicon as the Pico 2 W), 16 MB QSPI flash, and a USB-C connector. Build with:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DPICO_SDK_PATH=<sdk> -DWAVESHARE_RP2350B_PLUS_W_BUILD=ON
cmake --build build --target ds5-bridge
```

Or download precompiled firmware from GitHub Actions.

### USB Wake Feature (Wake-on-PS)

Wake-on-PS is built into the standard firmware — there is no separate `feat/usb-wake` branch or `ds5-bridge-wake.uf2`
build. It is **disabled by default**; turn it on with the **Wake PC from sleep on PS button** toggle in the
[web config](#configuration). When enabled, the dongle presents a HID keyboard interface and advertises USB remote
wakeup so a controller button can wake the PC from **S3 sleep**; when disabled, that interface is not enumerated. See
[Wake-on-PS](#wake-on-ps-optional) for setup. This is the upstream feature and is unchanged here; for waking a
**fully powered-off** PC, use this fork's [Wake-on-LAN](#wake-on-lan) instead.

It is recommended to read upstream issues #60 and #61 before using this feature.

## Wake-on-PS (optional)

Enabling the **Wake PC from sleep on PS button** toggle in the [web config](#configuration) makes the dongle present a
second HID interface (a boot keyboard) and advertise USB remote wakeup. A controller button press while the host is
suspended then injects an **F15** keypress, waking the PC from **S3 sleep**. F15 was chosen because it has no default
Windows or app binding — a stray fire never inserts characters or triggers shortcuts. The toggle is off by default, and
the keyboard interface is only enumerated while it (or the Xbox Game Bar shortcut) is enabled.

Scope: **S3 only.** Modern Standby (S0ix) is not supported. To check your machine, run `powercfg /a` — you need
"Standby (S3)" listed under available sleep states.

After enabling the toggle (then **Reconnect USB** so the interface re-enumerates):

1. Open Device Manager → the new **HID Keyboard Device** (and its parent **USB Composite Device**) → Properties → Power
   Management → tick **"Allow this device to wake the computer."**
2. Verify with `powercfg /devicequery wake_armed`.
3. Sleep the PC; press any button on the controller; the PC should wake within ~1 s.
4. After a wake, `powercfg /lastwake` should attribute the wake to the HID Keyboard Device.

> Wake also needs `SelectiveSuspendEnabled = 1` (a `REG_DWORD`) on the controller's audio interface (`MI_00`). Windows
> only writes it at first install, so a runtime toggle may need it set manually. It lives under each per-instance
> `Device Parameters` key:
>
> ```
> HKLM\SYSTEM\CurrentControlSet\Enum\USB\VID_054C&PID_0CE6&MI_00\<instance>\Device Parameters
>     SelectiveSuspendEnabled    (REG_DWORD) = 1
> ```
>
> `PID_0CE6` is the DualSense (`PID_0DF2` for the Edge), and `<instance>` is device/port-specific (e.g.
> `6&212078ea&1&0000`), so there can be more than one node — set it on every one. An elevated PowerShell one-liner that
> covers all present instances:
>
> ```powershell
> Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Enum\USB\VID_054C&PID_0CE6&MI_00' | ForEach-Object {
>   New-ItemProperty "$($_.PSPath)\Device Parameters" SelectiveSuspendEnabled -Value 1 -PropertyType DWord -Force }
> ```
>
> Then Reconnect USB or reboot. (Re-installing the device — clearing its Windows device cache and replugging — also
> makes Windows write the value itself.)

## Xbox Game Bar (optional)

The **PS button = Xbox Game Bar** toggle in the [web config](#configuration) maps the controller's PS button to
keyboard shortcuts, sent over the same HID keyboard interface used by [Wake-on-PS](#wake-on-ps-optional):

- **Short press** (tap and release) → `Win`+`G`, which opens the **Xbox Game Bar** overlay.
- **Long press** (hold ≥ 750 ms) → `Win`+`Tab`, which opens **Task View**.

The toggle is off by default, and the keyboard interface is only enumerated while it (or wake) is enabled.
> If the Game Bar overlay opens but does not respond to controller inputs, Windows may be missing the modern input stack. Installing or updating **Microsoft GameInput** will resolve this and restore controller navigation. You can install the service directly by opening an elevated command prompt and running `winget install Microsoft.GameInput`, or read the [official documentation](https://learn.microsoft.com/en-us/gaming/gdk/docs/features/common/input/overviews/input-overview) for more details.

## Roadmap

- Please check out the upstream [DS5Dongle plan](https://github.com/users/awalol/projects/5)

## Community

- Join the upstream Discord server: [Discord Server](https://discord.gg/hM4ntchGCa)
- If you have a bug in this fork, please open an issue here.

## Credits

This is a fork of [**awalol/DS5Dongle**](https://github.com/awalol/DS5Dongle); all of the
upstream work — the bridge itself, HD haptics, audio, the RAM-resident audio path, and the
BOOTSEL/web-config tooling — is theirs. This fork adds Wake-on-LAN and the related
memory/latency work described above.

## References

- [rafaelvaloto/Pico_W-Dualsense](https://github.com/rafaelvaloto/Pico_W-Dualsense) — Project inspiration
- [egormanga/SAxense](https://github.com/egormanga/SAxense) — Bluetooth Haptics POC
- [https://controllers.fandom.com/wiki/Sony_DualSense](https://controllers.fandom.com/wiki/Sony_DualSense) - DualSense
  data report structure documentation
- [Paliverse/DualSenseX](https://github.com/Paliverse/DualSenseX) — Speaker report packet
