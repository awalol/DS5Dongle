# Pico2W DualSense 5 Bridge

[中文](./README.md)

> Turn a Raspberry Pi Pico2W into a wireless adapter for the DualSense (DS5) controller.

## Overview

This project enables the Raspberry Pi Pico2W to function as a Bluetooth bridge for the DualSense controller, allowing wireless connectivity with enhanced haptics support.

## Features

- 🎮 Full DualSense connectivity via Pico2W
- 🔊 Supports HD haptics (advanced vibration feedback)
- 📡 Wireless Bluetooth bridging
- ⚙️ Adjustable haptic gain via microphone volume
- 🔕 Configurable LED and disconnection behaviors
- 📺 Optional Pico-OLED-1.3 status display (battery, BD address, live stick / button / D-pad / trigger visualization)

## Getting Started

### Flashing Firmware

1. Hold the BOOTSEL button on the Pico2W
2. Connect the Pico2W to your computer via USB
3. The device will mount as a USB storage device
4. Drag and drop the .uf2 firmware file onto the device

### Pairing the Controller

1. Put the DualSense controller into Bluetooth pairing mode
2. Wait for the Pico2W to detect and connect
3. Once connected, the device will appear on the host system

## Configuration

The following controller settings are repurposed:

### Microphone volume

Controls haptic gain multiplier

Range: [1.0 – 2.0]

### Speaker mute

Disables LED connection indicator

Takes effect after controller reconnects

### Microphone mute

Disables silent disconnection behavior

## Notes

The Pico device will only be visible to the system after the controller is connected

Some behaviors depend on reconnection cycles to take effect

## Known Issues

- ⚠️ Audio may experience slight stuttering
- ⚠️ Overclocking is required for proper performance

## Performance / Overclocking

Due to encoding requirements, the Pico2W must be overclocked:

Current settings:

- Voltage: 1.2V
- Frequency: 320 MHz

If your device fails to boot:

- Increase voltage slightly or Reduce CPU frequency

## Build Instructions

To build the project from source:

1. Update TinyUSB in the Pico SDK to the latest version
2. Compile using standard Pico SDK toolchain

## Optional Hardware: Pico-OLED-1.3

A Waveshare Pico-OLED-1.3 (128×64 SH1107) can be plugged on top of the Pico2W headers for a live status display. No firmware reconfiguration needed — the firmware drives it automatically when present, and gracefully ignores it when absent.

### What it shows

- Title bar with `[ON]` / `[--]` connection status
- Paired DualSense Bluetooth address
- Battery percentage with bar; suffix `+` when charging, `*` when fully charged, `!` on charge error
- Both analog stick positions (live)
- L1/R1 indicators, L2/R2 analog trigger fill bars
- D-pad direction (with diagonals)
- Face buttons (△ ◯ ✕ □) with pressed state

### On-board buttons

- **KEY0** — flashes the screen (placeholder for future screen-cycle)
- **KEY1** — sends a test rumble to the bonded DualSense (validates the firmware → controller path without a host)

### Pinout

The standard Waveshare Pico HAT layout is used:

| Function | GPIO |
|---|---|
| MOSI | 11 |
| SCK  | 10 |
| CS   | 9  |
| DC   | 8  |
| RST  | 12 |
| KEY0 | 15 |
| KEY1 | 17 |

## Roadmap

- Improve audio stability
- Multi-screen cycling on the OLED add-on (diagnostics / audio meters)
- Synthetic HD-haptic from basic rumble for games that don't send native haptic data

## References

- [rafaelvaloto/Pico_W-Dualsense](https://github.com/rafaelvaloto/Pico_W-Dualsense) — Project inspiration
- [egormanga/SAxense](https://github.com/egormanga/SAxense) — Bluetooth Haptics POC
- [https://controllers.fandom.com/wiki/Sony_DualSense](https://controllers.fandom.com/wiki/Sony_DualSense) - DualSense data report structure documentation
- [Paliverse/DualSenseX](https://github.com/Paliverse/DualSenseX) — Speaker report packet