# Pico2W DualSense 5 Bridge

[中文](./README.CN.md)

> Turn a Raspberry Pi Pico2W into a wireless adapter for the DualSense (DS5) controller.

## Overview

This project enables the Raspberry Pi Pico2W to function as a Bluetooth bridge for the DualSense controller, allowing wireless connectivity with enhanced haptics support.

## Features

- 🎮 Full DualSense connectivity via Pico2W
- 🔊 Supports HD haptics (advanced vibration feedback)
- 📡 Wireless Bluetooth bridging
- ⚙️ Adjustable haptic gain via microphone volume
- 🔕 Configurable LED and disconnection behaviors

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

### Low-battery LED indicator

When the connected DualSense reports its battery at or below 10% (and it is not charging), the Pico onboard LED switches from solid-on to a 1 Hz blink so you can see the warning at a glance. The LED returns to solid-on as soon as the controller is plugged in or its reported level rises again. The blink also fires when `disable_pico_led` is set — the warning is treated as critical and overrides the LED-off preference; the LED returns to its disabled (off) state once the battery recovers or the controller starts charging.

To opt out at build time, configure with `-DENABLE_BATT_LED=OFF`. Default is ON.

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

## Roadmap
- Please check out [DS5Dongle plan](https://github.com/users/awalol/projects/5)

## Community
- Join the Discord server: [Discord Server](https://discord.gg/hM4ntchGCa)
- If you have a bug, please open an issue instead.

## References

- [rafaelvaloto/Pico_W-Dualsense](https://github.com/rafaelvaloto/Pico_W-Dualsense) — Project inspiration
- [egormanga/SAxense](https://github.com/egormanga/SAxense) — Bluetooth Haptics POC
- [https://controllers.fandom.com/wiki/Sony_DualSense](https://controllers.fandom.com/wiki/Sony_DualSense) - DualSense data report structure documentation
- [Paliverse/DualSenseX](https://github.com/Paliverse/DualSenseX) — Speaker report packet
