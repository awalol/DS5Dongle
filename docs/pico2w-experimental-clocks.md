# Experimental Pico 2 W clock profiles

The normal Pico 2 W firmware runs at 150 MHz. The 225 and 300 MHz profiles are
opt-in, default-off experiments for `pico2_w` only. Both exceed the RP2350's
specified 150 MHz system clock limit. Neither profile is a supported release
configuration or a claim of electrical safety or reliable operation.

## Coupled settings

- Standard (both options off): 150 MHz system clock, CYW43 PIO divider `/2`,
  boot2 flash XIP divider `/2`, SDK-default RXDELAY `2`, and SDK-default core
  voltage handling.
- `DS5_PICO2W_EXPERIMENTAL_225MHZ=ON`: 225 MHz, nominal VREG 1.15 V, CYW43
  PIO `/3`, boot2 XIP `/3`, and RXDELAY `3`.
- `DS5_PICO2W_EXPERIMENTAL_300MHZ=ON`: 300 MHz, nominal VREG 1.15 V, CYW43
  PIO `/4`, boot2 XIP `/4`, and RXDELAY `4`.

`CMakeLists.txt` rejects simultaneous profile selection, other boards, and
conflicting compiler/board/clock overrides. `src/main.cpp` applies the
experimental VREG setting before raising the system clock. Keep the voltage,
CPU clock, CYW43 PIO divider, boot2 XIP divider, and RXDELAY together; matching
nominal peripheral rates does not establish safe operation.

## Build and inspect

Use the pinned dependencies in `.github/workflows/build-firmware.yml` (Pico SDK
2.3.0, TinyUSB `2d56dc533e45e4e91b15e93fdab5e22e964f328d`, and Arm GNU
Toolchain 15.2.Rel1). Initialize the repository submodules and use a separate
build directory for each profile. Replace the SDK path below with that pinned
checkout:

```sh
cmake -S . -B build/pico2w-oc225 -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_SDK_PATH=/path/to/pico-sdk-2.3.0 -DPICO_BOARD=pico2_w -DVERSION=local-oc225 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DDS5_PICO2W_EXPERIMENTAL_225MHZ=ON
cmake --build build/pico2w-oc225 --target ds5-bridge

cmake -S . -B build/pico2w-oc300 -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_SDK_PATH=/path/to/pico-sdk-2.3.0 -DPICO_BOARD=pico2_w -DVERSION=local-oc300 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DDS5_PICO2W_EXPERIMENTAL_300MHZ=ON
cmake --build build/pico2w-oc300 --target ds5-bridge
```

The CI firmware matrix does not build these profiles. Before using an image,
verify the configured board/profile, the effective-clock CMake line, and the
actual compile commands for `src/main.cpp`, the SDK CYW43 driver, and boot2's
`compile_time_choice.S`. Check the UF2 with `picotool`, record its SHA-256 and
dependency revisions, and compare a same-toolchain standard build. A successful
build, ELF/UF2 inspection, or short device run is not a stability or safety
qualification.

## Electrical limits

The [RP2350 datasheet](https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf)
specifies a 150 MHz system clock, a 1.16 V maximum operating DVDD supply
(Section 14.9.5), a 1.21 V absolute maximum DVDD rating (Section 14.9.1), and
up to +3% regulator deviation from the programmed voltage (Section 14.9.6).
The experimental nominal 1.15 V setting could therefore reach 1.1845 V,
above the operating maximum. That calculation is a tolerance bound, not a
measured voltage on any tested board. Frequency-divider choices and a booting
device do not remove this electrical risk.

## Bounded observations and retired profile

As of 2026-09-29, the 225 MHz profile has build/static evidence but no
recorded device trial. A Pico 2 W running a 300 MHz image built from firmware
commit `3bf85ad` (UF2 SHA-256
`2214971eb068bed77818a9a940210fc6638a389346f5f0ef509284de966ba6a8`)
connected to one DualSense Edge: F8 returned the expected firmware version,
Windows enumerated its USB composite/HID interfaces after the controller
connected, and 20/20 complete 64-byte USB HID input reads (report ID plus
63-byte state) succeeded. A focused battery lighting check also ran.
Immediately after the UF2 copy, USB enumeration was
not observed in the first roughly 10 seconds; the later connection does not
establish immediate startup reliability. The USB reads may repeat a cached
Bluetooth state. No long-duration, audio, suspend/wake, other-controller, or
electrical safety qualification follows from this run. The detailed attempt
record is local and ignored at `artifacts/battery-four-tier-oc300-20260929.json`;
this tracked summary does not imply that record is present in a fresh clone.

The former 375 MHz / nominal 1.20 V profile was retired after a user-reported
no-boot test on 2026-09-28. The board revision, attempt count, logs, and cause
were not established. Its former UF2 SHA-256 was
`954e9c6ed86c477ce5feaeb78576f8086eb27e88c5d638a201e129ee03d2f200`.
Nominal 1.20 V already exceeds the operating maximum; at +3% it could reach
1.236 V, above the absolute maximum. The local failure record and quarantined
image remain under ignored `artifacts/`; they are not part of a fresh clone or
release. Do not restore or distribute that profile.

Keep all experimental UF2s and their provenance separate from the standard
three-asset release package.
