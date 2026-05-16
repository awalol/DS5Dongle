# Pico2W DualSense 5 Bridge

[中文](./README.md) · [Changelog](./CHANGELOG.md)

> Turn a Raspberry Pi Pico2W into a wireless adapter for the DualSense (DS5) controller.

## Overview

This project enables the Raspberry Pi Pico2W to function as a Bluetooth bridge for the DualSense controller, allowing wireless connectivity with enhanced haptics support.

## Features

- 🎮 Full DualSense connectivity via Pico2W
- 🔊 Supports HD haptics (advanced vibration feedback)
- 📡 Wireless Bluetooth bridging
- ⚙️ Adjustable haptic gain via microphone volume
- 🔕 Configurable LED and disconnection behaviors
- 📺 Optional Pico-OLED-1.3 status display with 6 screens: status, diagnostics, trigger test, gyro tilt, touchpad, and lightbar color picker

## Hardware

### Required

| Item | Notes | Approx. price |
|---|---|---|
| **Raspberry Pi Pico 2 W** | RP2350 MCU with on-board CYW43 Bluetooth/WiFi. [Official product page](https://www.raspberrypi.com/products/raspberry-pi-pico-2/) | ~$7 USD |
| **Sony DualSense Controller** | Any standard PS5 DualSense (VID `054C:0CE6`). | (existing) |
| **USB-C cable** | Connects the Pico 2 W to the host PC. | — |

### Optional (strongly recommended)

| Item | Notes | Approx. price |
|---|---|---|
| **Waveshare Pico-OLED-1.3** | 128×64 SH1107 OLED add-on board (SKU HIPI1798). Plugs directly onto the Pico 2 W headers. Firmware drives it automatically when present and ignores it when absent. [Product page](https://www.waveshare.com/pico-oled-1.3.htm) · [Wiki](https://www.waveshare.com/wiki/Pico-OLED-1.3) | ~$6 USD |
| **Small heatsink** for the RP2350 | The firmware overclocks the MCU to 320 MHz at 1.20 V — this is load-bearing for the CYW43 BT clock and not just for raw performance. A small heatsink or thermal pad helps under sustained gameplay. | $1–3 USD |

### Where to buy

Both the Pico 2 W and the Waveshare Pico-OLED-1.3 are widely available worldwide:

- **Adafruit**, **Pimoroni**, **The Pi Hut**, **DigiKey**, **Mouser** — major electronics distributors (US / EU)
- **Waveshare's own store** for the OLED add-on
- Regional **Amazon** storefronts — search `Raspberry Pi Pico 2 W` and `Waveshare Pico-OLED-1.3` (or the SKU `HIPI1798`)
- **AliExpress** — original Waveshare and Pico stock plus clones; check seller ratings

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

- ⚠️ Overclocking (320 MHz at 1.20 V) is required for stable BT operation — not just for performance. The CYW43 PIO SPI clock is derived from sys_clk; the project's `CYW43_PIO_CLOCK_DIV_INT=4` only yields a workable SPI rate at 320 MHz, and the RP2350 needs the voltage bump to be stable at that clock. See [Performance / Overclocking](#performance--overclocking) for details.

> **Note:** The "audio may experience slight stuttering" issue noted in earlier versions was a 3 KB stack overflow in the Opus encoder path on core 1 (a `ResampleOut` buffer sized for 200 floats but written with up to 960). Fixed in this branch's audit pass — audio should now be clean at the same overclock.

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

## OLED Display Add-on (Pico-OLED-1.3)

Hardware purchase info is in [Hardware](#hardware). What the firmware does with the display:

### Boot splash (1.5 s on power-on)

```
┌──────────────────────────────┐
│                              │
│         DS5 Bridge           │
│          v0.5.4              │
│       Pico2W + OLED          │
│                              │
└──────────────────────────────┘
```

### Six screens, cycled with KEY0

#### 1. Status

Connection state, paired DualSense BD address, battery percentage with bar (`+` charging / `*` complete / `!` error), live analog stick positions, D-pad indicator, face buttons (△ ◯ ✕ □), L1/R1 indicators, L2/R2 analog trigger fill bars.

```
┌──────────────────────────────┐
│ DS5 Bridge v0.5.4      [ON]  │
│ 14:3A:9A:FF:D9:F9            │
│ Batt: 87%+   ████████░░░░░░  │
│                              │
│ ┌────┐  L1    △     R1 ┌────┐│
│ │ ·• │  L2  ○   □  R2  │ ·• ││
│ │    │   ▌    ✕     ▌  │    ││
│ └────┘                  └────┘│
└──────────────────────────────┘
  └ L-stick    └ D-pad/face btns
            └ vertical bars = L2/R2 analog triggers
```

When no controller is paired, the status line shows `[--]` and the body shows `(waiting for DS5)`.

#### 2. Diagnostics

Uptime since boot, HCI command-send error count (from the `HCI_LOG_CMD` macro), audio FIFO drop count, Opus FIFO drop count, BT state.

```
┌──────────────────────────────┐
│ Diagnostics                  │
│ Up: 0h 14m 22s               │
│ HCI errs:    0               │
│ Aud drops:   0               │
│ Opus drops:  0               │
│ BT: connected                │
│                              │
│ K0=next K1=rumble            │
└──────────────────────────────┘
```

#### 3. Trigger Test

KEY1 cycles through seven adaptive trigger effects applied to both L2 and R2. Pull each trigger to feel the effect — resistance, snap point, vibration patterns, etc.

```
┌──────────────────────────────┐
│ Trigger Test                 │
│ Mode: Weapon                 │
│ L2: 127   R2:  42            │
│                              │
│ ████░░░░░░     ██░░░░░░░░    │
│  (L2 pull)      (R2 pull)    │
│                              │
│ K0=next K1=cycle             │
└──────────────────────────────┘
```

Cycle order: **Off → Feedback → Weapon → Vibration → Bow → Gallop → Machine Gun → Off …**

Trigger effect bytes follow [dualsensectl](https://github.com/nowrep/dualsensectl)'s reverse-engineering with bitpacked 10-zone arrays, all at max strength so the effect is clearly felt.

#### 4. Gyro Tilt

Live X/Y/Z accelerometer values with a 40×40 crosshair box. Tilt the controller and watch the dot move in real time.

```
┌──────────────────────────────┐
│ Gyro Tilt                    │
│ X +123  Y -456  Z +8123      │
│         ┌────────┐           │
│         │   │    │           │
│         │───•────│           │
│         │   │    │           │
│         └────────┘           │
└──────────────────────────────┘
```

#### 5. Touchpad

Live render of the touchpad surface. Dots appear at the current finger positions; the finger count below the box updates as fingers touch / leave.

```
┌──────────────────────────────┐
│ Touchpad                     │
│ ┌──────────────────────────┐ │
│ │    •              •      │ │
│ │                          │ │
│ └──────────────────────────┘ │
│ Fingers: 2                   │
│                              │
│ K0=next                      │
└──────────────────────────────┘
```

#### 6. Lightbar Color Picker

Live RGB preview on the controller's actual lightbar. While on this screen, the firmware sends the tilt-derived color to the DualSense at 10 Hz so the lightbar IS the visual preview (the OLED is monochrome).

```
┌──────────────────────────────┐
│ Lightbar        [LIVE]       │
│ R:128 G: 77 B:200            │
│ ████░░░  ██░░░░░  ██████░░   │
│  (R)     (G)      (B)        │
│ Sv: T=0 C=1 X=2 S=3          │
│ Tilt = R/G/B                 │
│ K0=next K1=cycle             │
└──────────────────────────────┘
```

- Tilt the controller on each axis to dial in R / G / B
- Press **△ ◯ ✕ □** on the controller to save the current color into **favorite slot 0 / 1 / 2 / 3**
- Press **KEY1** to cycle the mode tag: `[LIVE]` → `[FAV0]` → `[FAV1]` → `[FAV2]` → `[FAV3]` → back to `[LIVE]`
- Default favorites: Red, Green, Blue, White

### KEY1 behavior by screen

| Screen | KEY1 action |
|---|---|
| Status, Diagnostics, Gyro Tilt, Touchpad | Send a 250 ms test-rumble burst to the DualSense (validates firmware → controller path without needing a host) |
| Trigger Test | Cycle the trigger effect preset |
| Lightbar Color Picker | Cycle between LIVE preview and the 4 favorite slots |

### Pinout (standard Waveshare Pico HAT layout)

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