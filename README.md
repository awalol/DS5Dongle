# Pico2W DualSense 5 Bridge

[中文](./README.CN.md)

> Turn a Raspberry Pi Pico2W (or other compatible board) into a wireless adapter for the DualSense (DS5) controller.

***This repository only implements the core functionality of DS5Dongle — making a wireless controller appear as a wired
connection. For additional features, please refer to [Community Fork](#Community-Fork)***

## Overview

This project enables the Raspberry Pi Pico2W (or other compatible board, e.g. the Waveshare RP2350B-Plus-W) to function as a Bluetooth bridge for the DualSense controller, allowing wireless connectivity with enhanced haptics support.

## Features

- 🎮 Full DualSense connectivity via Pico2W (or other compatible board)
- 🔊 Supports HD haptics (advanced vibration feedback)
- 🎧 Headset audio output — controller speaker and 3.5 mm jack
- 🎤 Headset microphone input — the controller mic is exposed as a USB audio input device
- 📡 Wireless Bluetooth bridging
- ⚡ Runs at the stock 150 MHz clock — no overclock required

> **Microphone note:** the controller mic shows up as a USB audio input —
> "Headset Microphone" on Windows. After selecting it as your recording device,
> raise its input/capture level in your OS — Windows in particular often defaults
> it to 0 (or very low), which makes the mic seem dead even though it's working.

## Getting Started

### Get the firmware

You have two options:

- **Download a pre-built `.uf2`** — grab the newest
  [Releases](../../releases) build (`ds5-bridge-*.uf2`). No tools needed.
- **Build it yourself** — see [Build Instructions](#build-instructions)
  below (Windows users get a one-command script).

### Flashing Firmware

1. Hold the BOOTSEL button on the Pico2W
2. Connect the Pico2W to your computer via USB
3. The device will mount as a USB storage device
4. Drag and drop the .uf2 firmware file onto the device

### Pairing the Controller

1. Put the DualSense controller into Bluetooth pairing mode
2. Wait for the Pico2W to detect and connect
3. Once connected, the device will appear on the host system

***You may need to replug the Pico when the controller is in pairing mode.***

## Configuration

You can modify the Pico settings via the web config.

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

## Notes

The Pico device will only be visible to the system after the controller is connected

Some behaviors depend on reconnection cycles to take effect

### Low-battery LED indicator

When the connected DualSense reports its battery at or below 10% (and it is not charging), the Pico onboard LED switches
from solid-on to a 1 Hz blink so you can see the warning at a glance. The LED returns to solid-on as soon as the
controller is plugged in or its reported level rises again. The blink also fires when `disable_pico_led` is set — the
warning is treated as critical and overrides the LED-off preference; the LED returns to its disabled (off) state once
the battery recovers or the controller starts charging.

To opt out at build time, configure with `-DENABLE_BATT_LED=OFF`. Default is ON.

### Pico W Version

Pico W only has haptics support, no speaker. You can enable Pico W firmware compilation with `-DPICO_W_BUILD=ON`, or
download precompiled firmware from GitHub Actions.

### Waveshare RP2350B-Plus-W

The [Waveshare RP2350B-Plus-W](https://www.waveshare.com/wiki/RP2350B-Plus-W) is an RP2350B-based board with the RM2 wireless module (same CYW43 silicon as the Pico 2 W), 16 MB QSPI flash, and a USB-C connector. Build with:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DPICO_SDK_PATH=<sdk> -DWAVESHARE_RP2350B_PLUS_W_BUILD=ON
cmake --build build --target ds5-bridge
```

Or download precompiled firmware from GitHub Actions.

### USB Wake Feature

This feature is experimental. If you need this functionality, please check out the feat/usb-wake branch to compile it,
or use the precompiled firmware from GitHub Actions under that branch. The `ds5-bridge-wake.uf2` is the firmware with
this feature enabled.

It is recommended to read #60 and #61 before using this feature.

## Known Issues

- ⚠️ Audio may experience slight stuttering

## Performance

The audio path — libopus encode/decode, the resampler, and the Bluetooth/USB
packet handling on the hot path — executes from **RAM** instead of flash. This
removes flash-fetch (XIP cache miss) stalls from the time-critical audio loop,
which previously forced the RP2350 to be overclocked just to keep up with audio
encoding.

As a result, the firmware runs the **full audio path (haptics, speaker, 3.5 mm
output, and microphone) at the stock 150 MHz clock — no overclock and no
core-voltage bump.**

> Earlier releases required 320 MHz @ 1.2 V; overclocking is no longer needed.
> If you build for a different board and it fails to boot, reduce the CPU
> frequency (and/or raise the voltage) in `CMakeLists.txt`.

## Build Instructions

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

Build a variant with `-Variant debug` or `-Variant wake`.

### Other platforms

To build from source manually:

1. Install the Pico SDK 2.2.0 and switch its TinyUSB submodule to tag 0.20.0
   i.e. ***Update TinyUSB in the Pico SDK to the latest version***
2. Initialise this repo's submodules: `git submodule update --init --recursive`
3. Configure and build with the standard Pico SDK toolchain:
   `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_SDK_PATH=<sdk>`
   then `cmake --build build --target ds5-bridge`

1. ***Update TinyUSB in the Pico SDK to the latest version***
2. Compile using standard Pico SDK toolchain

On macOS, `tools/build-macos.sh` can prepare a repo-local Pico SDK checkout, prompt to install missing Homebrew build
tools, initialize submodules, pin TinyUSB, and build the wake firmware:

```sh
tools/build-macos.sh
```

Use `tools/build-macos.sh --standard` for the non-wake firmware, `--clean` to rebuild from scratch, or
`--sdk-dir <path>` to use an existing SDK checkout. When using `--sdk-dir`, the script asks before checking that SDK out
to the required Pico SDK and TinyUSB versions. If Homebrew's `arm-none-eabi-gcc` formula is installed without standard C
headers, the script asks to install the complete `gcc-arm-embedded` cask and points CMake at that toolchain.

## Wake-on-PS (optional)

A `-DENABLE_WAKE_HID=ON` build adds a second HID interface (a boot keyboard) that injects an **F15** keypress when any
controller button is pressed while the host is suspended, waking the PC from **S3 sleep**. F15 was chosen because it has
no default Windows or app binding — a stray fire never inserts characters or triggers shortcuts.

Scope: **S3 only.** Modern Standby (S0ix) is not supported. To check your machine, run `powercfg /a` — you need "
Standby (S3)" listed under available sleep states.

After flashing the wake build:

1. Open Device Manager → the new **HID Keyboard Device** (and its parent **USB Composite Device**) → Properties → Power
   Management → tick **"Allow this device to wake the computer."**
2. Verify with `powercfg /devicequery wake_armed`.
3. Sleep the PC; press any button on the controller; the PC should wake within ~1 s.
4. After a wake, `powercfg /lastwake` should attribute the wake to the HID Keyboard Device.

## Roadmap

- Please check out [DS5Dongle plan](https://github.com/users/awalol/projects/5)

## Community

- Join the Discord server: [Discord Server](https://discord.gg/hM4ntchGCa)
- If you have a bug, please open an issue instead.

## References

- [rafaelvaloto/Pico_W-Dualsense](https://github.com/rafaelvaloto/Pico_W-Dualsense) — Project inspiration
- [egormanga/SAxense](https://github.com/egormanga/SAxense) — Bluetooth Haptics POC
- [https://controllers.fandom.com/wiki/Sony_DualSense](https://controllers.fandom.com/wiki/Sony_DualSense) - DualSense
  data report structure documentation
- [Paliverse/DualSenseX](https://github.com/Paliverse/DualSenseX) — Speaker report packet
