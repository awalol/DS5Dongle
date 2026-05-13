# Pico2W DualSense 5 Bridge

[中文](./README.CN.md)

> Turn a Raspberry Pi Pico2W into a wireless adapter for the DualSense (DS5) controller.

## Overview

This project enables the Raspberry Pi Pico2W to function as a Bluetooth bridge for the DualSense controller, allowing wireless connectivity with enhanced haptics support.

## Features

- 🎮 Full DualSense connectivity via Pico2W
- 📺 **OLED Multi-Slot UI**: Hardware display module for pairing management, slot isolation, and detailed battery metrics.
- 🔊 Supports HD haptics (advanced vibration feedback)
- 📡 Wireless Bluetooth bridging
- ⚙️ Adjustable haptic gain via microphone volume
- 🔕 Configurable LED and disconnection behaviors

## OLED Multi-Slot System (Hardware Mod)

This repository includes support for an optional **Waveshare 1.3-inch OLED (SH1107)** display to provide a graphical UI for managing multiple controllers. Because the Pico 2W Bluetooth chip has a hardcoded MAC address, true "virtual dongles" are impossible. Instead, this mod implements strict software-level slot isolation using BTstack's L2CAP and HCI handlers.

### Required Hardware
- **Raspberry Pi Pico 2W**
- **Waveshare 1.3-inch OLED (SH1107)** (SPI version)
- **2x Momentary Push Buttons**

### Wiring & Pins
| Component | Pin | Pico 2W GPIO |
| :--- | :--- | :--- |
| **OLED** | `DIN` | GP11 (SPI1 TX) |
| | `CLK` | GP10 (SPI1 SCK) |
| | `CS` | GP9 (SPI1 CSn) |
| | `DC` | GP8 |
| | `RST` | GP12 |
| **Buttons** | `Next` | GP15 (Connect to GND, Pull-up enabled in code) |
| | `Wipe` | GP17 (Connect to GND, Pull-up enabled in code) |

*Note: The display is intended to be mounted vertically (rotated 90 degrees), with the physical buttons located at the bottom.*

### OLED Features
* **4-Slot Memory**: Safely stores 4 different controller pairings in flash memory. Prevents "ghost" controllers from hijacking your active connection.
* **High-Detail Pixel Art UI**: Crisp 16x16 pixel art icons for Gamepad, Hourglass (Scanning/Waiting), and Soft-Keys.
* **Accurate Battery Monitoring**: Parses raw DualSense reports (Byte 55) to show real-time 0-100% battery and charging status directly on the screen.
* **Instant Slot Switching**: Press the `Next` button (GP15) to cycle slots. The firmware safely handles the "Zombie Link" disconnect process to ensure instant switching.
* **Factory Reset**: Hold the `Wipe` (GP17) button for 3 seconds to wipe all 4 slots and clear the Bluetooth pairing NVRAM.

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

## Wake-on-PS (optional)

A `-DENABLE_WAKE_HID=ON` build adds a second HID interface (a boot keyboard) that injects an **F15** keypress when any controller button is pressed while the host is suspended, waking the PC from **S3 sleep**. F15 was chosen because it has no default Windows or app binding — a stray fire never inserts characters or triggers shortcuts.

Scope: **S3 only.** Modern Standby (S0ix) is not supported. To check your machine, run `powercfg /a` — you need "Standby (S3)" listed under available sleep states.

After flashing the wake build:

1. Open Device Manager → the new **HID Keyboard Device** (and its parent **USB Composite Device**) → Properties → Power Management → tick **"Allow this device to wake the computer."**
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
- [https://controllers.fandom.com/wiki/Sony_DualSense](https://controllers.fandom.com/wiki/Sony_DualSense) - DualSense data report structure documentation
- [Paliverse/DualSenseX](https://github.com/Paliverse/DualSenseX) — Speaker report packet
