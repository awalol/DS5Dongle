# Pico2W DualSense 5 Bridge
[English](./README.EN.md)
> Turn a Pico2W into a wireless adapter for the DS5 controller

# Features
 - Supports HD haptics

# Usage
1. Hold BOOTSEL on the Pico to enter flashing mode
2. Drag the .uf2 file onto it
3. Put the DS5 controller into Bluetooth pairing mode
4. Enjoy it

- Adjust microphone volume to change the haptic gain multiplier, range [1, 2]
- Enable speaker mute to disable the LED connection indicator (takes effect after the controller reconnects)
- Enable microphone mute to disable silent disconnection
- The system will only show the device after the controller has connected to the Pico

# Known Issues:
- Audio may have slight stuttering
- Due to encoding requirements, the Pico must be overclocked. Current settings are 1.2V at 320 MHz.
- If your Pico will not boot at these overclock settings, increase the voltage or lower the frequency

# Roadmap

# Build
The TinyUSB version inside the Pico SDK must be upgraded to the latest.

# Acknowledgements
 - [rafaelvaloto/Pico_W-Dualsense](https://github.com/rafaelvaloto/Pico_W-Dualsense) - Inspiration
 - [egormanga/SAxense](https://github.com/egormanga/SAxense) - Haptic packets
 - [https://controllers.fandom.com/wiki/Sony_DualSense](https://controllers.fandom.com/wiki/Sony_DualSense) - Data packet structure
 - [Paliverse/DualSenseX](https://github.com/Paliverse/DualSenseX) - Speaker data packets
