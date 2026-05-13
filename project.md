# DS5Dongle Multi-Slot Profile System

## Overview
This project adds a Waveshare 1.3-inch OLED display (SH1107) to the DS5Dongle Pico 2W firmware, serving as a UI to manage a multi-controller profile system. Because the Pico W's CYW43439 Bluetooth chip has a hardcoded MAC address, hardware-level "virtual dongles" are impossible. Instead, we implemented strict software-level slot isolation using BTstack's L2CAP and HCI event handlers.

## Hardware & Pins
* **OLED SPI (SH1107)**: `DIN 11`, `CLK 10`, `CS 9`, `DC 8`, `RST 12`
* **Buttons**: 
  * `KEY0 (Pin 15)`: Cycle through Slots 1-4.
  * `KEY1 (Pin 17)`: Hold for 3 seconds to Factory Reset (Wipes all slots and BTstack pairing NVM).

## Core Features & Fixes Implemented

### 1. Vertical OLED UI & Orientation
* **90-Degree Rotation**: The display is rotated 90 degrees clockwise to move the hardware buttons (GP15/GP17) to the bottom of the device.
* **Professional Layout**: Implemented a tight **6x9 character spacing** (5px font + 1px gap) for a crisp, high-density look.
* **Header & Labels**: Centered "SLOT X" header and "NX" / "DEL" soft-key labels positioned directly above the physical buttons.
* **Pure HEX Rendering**: Fixed a font corruption bug by ensuring the character bitmask table uses pure, verified hexadecimal values.

### 2. Battery Monitoring
* **Accurate Parsing**: Real-time battery status is extracted from **Byte 55** of the DS5 Bluetooth Input Report.
* **Hardware Scale**: Maps the DualSense's internal **0-8 battery level** scale to a user-friendly 0-100% percentage.
* **Charging Status**: Detects and displays `CHG` when the controller is plugged into USB.

### 3. Multi-Slot Isolation Logic
* **Flash Memory Persistence**: Slot assignments (`slot_addrs` and `slot_occupied`) are persistently saved to the Pico's Flash Memory (`FLASH_TARGET_OFFSET`).
* **L2CAP Rejection**: To prevent "ghost" controllers from hijacking empty slots, `L2CAP_EVENT_INCOMING_CONNECTION` strictly rejects incoming connections unless the MAC matches the active slot, OR if the slot is empty and a `new_pair` is actively occurring.
* **Security Event Handlers**: `HCI_EVENT_USER_CONFIRMATION_REQUEST` is secured to ensure that only authorized MAC addresses for the current slot can refresh their pairing.

### 4. Stability & Bug Fixes
* **Fixed Random Reboots (Multicore Lockout)**: Implemented `multicore_lockout` to safely pause Core 1 (audio processing) during Flash writes, preventing CPU Hard Faults.
* **Fixed 30-Second Disconnect Delay (Pending Slot Switch)**: Implemented a state machine that ensures the radio is fully disconnected from an old slot before internal variables switch to the new slot.
* **Fixed The SDP Disconnect Bug**: Corrected the `L2CAP_EVENT_CHANNEL_CLOSED` logic to ignore temporary SDP query closures that were previously causing premature link drops.
* **Fixed Reconnection Race Conditions**: The Pico now intelligently waits for the controller to drive the L2CAP reconnection handshake to avoid packet collisions.

## Current Status
* **Verified**: Multi-slot switching, vertical UI rotation, battery monitoring, and reconnection are all fully operational and stable.
* **Refactored**: Codebase is clean, optimized for the vertical aspect ratio, and ready for production submission.