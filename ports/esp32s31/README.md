# ESP32-S31 port

Target board: the **Maple ESP32S31 Alef** (AnalogLamb / XPU Labs), a
dongle-style breakout with the S31's high-speed USB on a Type-C port. The
**Espressif ESP32-S31-Function-CoreBoard-1** runs the same image -- see
[Also runs on](#also-runs-on-esp32-s31-function-coreboard-1) -- and is where the
port was developed.

Verified on hardware on the Alef: pairing, reconnect after a PS-hold and after a
power-off, USB enumeration at high speed, speaker and microphone audio (recordings
checked, speaker and microphone together), rumble, input, the lightbar handover on
a fresh pair, a PS-hold reconnect and a power-off, and the board LED following
the controller's colour. Earlier, on the Function-CoreBoard-1: pairing, reconnect
after both a PS-hold power-cycle and a dongle replug, and speaker audio under
sustained load. The CoreBoard-1's microphone result was taken at full speed,
before this target moved to high speed, and does not carry over: the high-speed
microphone needed the isochronous IN fix described under "High speed". The Alef
carries the same module wired to the same pins, so nothing in the firmware
differs between the two; only the cabling does.

The Pico2W / Waveshare RP2350B builds are unaffected; they still build from the
top-level `CMakeLists.txt` and were verified unchanged by this work.

## The Alef board

| | |
| --- | --- |
| Module | ESP32-S31-WROOM-3: 16 MB quad flash, 16 MB octal PSRAM |
| BOOT button | IO61 to GND, active low |
| Status LED | SK6805 addressable RGB, data on IO60, 3.3 V |
| `HOST` Type-C | the S31's USB OTG (UTMI high-speed PHY) -- **the PC cable goes here** |
| `UART` Type-C | CH343P USB-to-UART with auto-download -- flashing and the console |

Pin assignments are from the schematic excerpts in the vendor's user guide
(`xpulabs/docs_en`, `maple_esp32/src/maple_esp32s31`): BOOT1 on IO61, D2
(SK6805-2427) DIN on IO60, and the module's `USB_DP`/`USB_DM` on the `HOST`
connector. They match `src/port/esp32s31/board_esp32s31.h`, which is why one
image serves both boards.

### Cabling

- **As a dongle:** one cable from the PC to the `HOST` port. That port also feeds
  the board's 3.3 V regulator, so it powers the board and carries the controller
  data over the high-speed peripheral. Use a USB-A-to-C cable to start with: a
  Type-A host port supplies VBUS unconditionally, whereas a C-to-C cable only
  gets VBUS if the board presents Rd on CC, and the published material does not
  show the `HOST` port's CC network.
- **To flash or watch the console:** the `UART` port. The CH343P's auto-download
  circuit straps IO61 itself, so `idf.py flash` needs no button presses.
- **Not both into a PC at once.** The vendor's block diagram routes the `UART`
  port's 5 V to the `HOST` port, the arrangement that lets `HOST` power a device
  in host mode. With both cabled to computers, that path can drive 5 V back into
  the PC's port. Debug with the `UART` port alone, or power the board from a
  charger rather than a second PC port.

The S31 has no FS/LS PHY (`soc_caps.h`: `SOC_USB_UTMI_PHY_NUM 1`,
`SOC_USB_FSLS_PHY_NUM 0`), and for the UTMI PHY device vs host is purely a
software choice: `usb_phy.c`'s set-mode path disconnects the D+/D- pulldowns for
device mode and returns, with no ID pin or VBUS-valid signal involved. The
`HOST` silkscreen describes the vendor's intended use, not a hardware role;
`board_init()` selects `USB_OTG_MODE_DEVICE`.

With no VBUS sense wired, the device attaches whenever it is powered rather than
when a host appears. On the Alef, powered from `HOST`, those are the same event.

## Also runs on: ESP32-S31-Function-CoreBoard-1

Same image, different cable. On the CoreBoard-1 `USB_DP`/`USB_DM` go to the
Type-A receptacle (Port 4); neither Type-C reaches OTG (Port 5 is the
fixed-function USB Serial/JTAG block, Port 6 a USB-to-UART bridge). That Type-A
port **sources 5 V** through U5, a TPS2051C whose active-high `EN` is strapped to
`VCC_5V` by R25 (10k, populated; the R29 pull-down is not fitted) and reaches no
GPIO, so firmware cannot switch it off. Verified on hardware: a USB mouse plugged
into that port powers up.

So on that board:

1. Use an A-to-A cable with **VBUS isolated**. Tape over pin 1 (the VBUS contact)
   of one plug rather than cutting it. Off-the-shelf "USB data blockers" do the
   opposite, passing power and blocking data.
2. Power the board from either Type-C port.

The TPS2051C's reverse blocking protects the board from host VBUS. It does
nothing to stop the board driving 5 V out into the host port, which is the
direction that matters. The hardware alternative to taping is removing R25 and
fitting R29, which straps `EN` low.

A single-PC-port Y-cable also works there: D+, D- and GND from the PC to the
Type-A port (no VBUS), and VBUS/GND from the same plug to the J2 5 V header
(pins 39/40, GND 37/38). Watch the 500 mA budget (that board has an Ethernet
PHY), and do not leave a Type-C plugged in at the same time.

Bit-banging USB over GPIO is not an alternative on either board. Low speed has
no isochronous endpoints, so the audio class is impossible and the 64-byte DS5
reports do not fit; full speed needs NRZI, bit stuffing and CRC at ~20 CPU
cycles per bit, which the RP2040 only manages through PIO, and the S31 has no
equivalent.

The SoC never drives VBUS on either board: `otg_io_conf` is left null, so
`drvvbus` is never routed out -- the self-powered-device shape IDF documents as
`USB_PHY_SELF_POWERED_DEVICE()`.

## Why the ESP32-S31 at all

The DualSense speaks Bluetooth **Classic** (BR/EDR) HID, which rules out most of
the line: the S3, C3, C6 and H2 are BLE-only, and the original ESP32 has BR/EDR
but no USB device peripheral. The S31 is the first Espressif part with both.

Confirmed in ESP-IDF master rather than assumed:
`components/soc/esp32s31/include/soc/soc_caps.h` defines
`SOC_BT_CLASSIC_SUPPORTED (1)`, and the controller ships a prebuilt
`libbredr_app.a` under `components/bt/controller/lib_esp32s31/`.

## Build

```sh
idf.py --preview -C ports/esp32s31 set-target esp32s31
idf.py --preview -C ports/esp32s31 build
```

`--preview` is required: esp32s31 is still a preview target in ESP-IDF.

Needs ESP-IDF **master** (verified against v6.2.0, commit 08e0d30a) — esp32s31
is not in a tagged release. Note that `install.sh` pulls openocd, which needs a
system `libusb-1.0`; openocd is only for JTAG debugging and the build works
without it.

## BOOT button

GPIO61, not GPIO0 -- see `src/port/esp32s31/board_esp32s31.h`. The gesture set is
shared with the Pico build and unchanged, except that there is no third gesture:

| Gesture | Action |
| --- | --- |
| 1 click | pair / switch controller (fresh inquiry) |
| 2 clicks | reboot |
| 3 clicks | nothing (Pico: reboot to BOOTSEL) |
| hold ~1.5 s | clear all pairings, six LED flashes to confirm |

Triple click is dropped rather than remapped, because the RP2350 gesture is
buying something this board does not need. On the Pico, BOOTSEL has to be held
*during* power-up, so a firmware call into the bootrom saves an unplug/replug. On
the ESP32-S31 the host tool straps the chip into download boot by itself over
DTR/RTS, so nothing is saved. It deliberately does not fall through to a reboot
either: a mis-counted click would then drop the controller link.

Nor could it be implemented honestly here. Download boot exists in S31 silicon --
there is an `EFUSE_DIS_FORCE_DOWNLOAD` bit to switch it off, which would be
pointless otherwise -- but the register that forces it from software is not
exposed for this target anywhere in ESP-IDF master. The only IDF code that does
this (`esp_usb_cdc_rom_console`) writes
`RTC_CNTL_OPTION1_REG.FORCE_DOWNLOAD_BOOT`, and the S31 has no `rtc_cntl_reg.h`
at all, nor an `LP_AON` equivalent. Guessing an address on preview silicon is how
boards get wedged, so `port::has_bootloader_reboot()` returns false here. If a
later IDF exposes the register, implement `reboot_to_bootloader()` and flip that
one function to true to get parity back.

## Status

Builds clean: `ds5-bridge-esp32s31.bin`, about 1 MB, with the BR/EDR controller and
BTstack Classic linked in.

Working:

- `src/port/` platform abstraction, Pico backend and ESP32-S31 backend. The Pico
  firmware grows 424 bytes of text against unmodified master — the abstraction
  is effectively free.
- Config storage on `esp_partition` with a RAM shadow (IDF cannot erase a
  mapped partition, so the RP2350's XIP-pointer trick does not carry over),
  `esp_timer` one-shots, GPIO61 boot button, RMT-driven RGB status LED (WS2812 on the
  CoreBoard-1, SK6805 on the Alef) that shows the controller's lightbar colour once
  connected (`port::led_set_on_color()`), task
  watchdog, USB PHY bring-up, core-1 audio worker as a pinned static task.
- BTstack built from `lib/btstack` (pinned to the same commit the Pico SDK
  uses) with the on-die controller in BR/EDR-only, controller-only mode.
  Verified in the generated sdkconfig: `CONFIG_BT_CTRL_BREDR_ENABLE=y`.
- HCI transport over VHCI (`src/port/esp32s31/bt_transport_esp32s31.cpp`),
  driving the pollable embedded run loop from the superloop. See below.
- This target enumerates at HIGH speed, matching the real DualSense (confirmed
  from its descriptors on hardware) rather than the RP2350, which has no
  high-speed PHY. The S31's only PHY is high-speed, so this is also the
  path of least resistance -- see "High speed" below for what it required.
  The descriptors are the Pico build's except for `bInterval`, which
  `ep_interval()` in `src/usb_descriptors.cpp` re-encodes per speed: at high
  speed it is an exponent, so the 1 ms audio and HID endpoints carry 4 rather
  than 1. Everything else is byte-for-byte identical and worth re-checking if
  either side is touched:

      riscv32-esp-elf-objdump -s --start-address=<sym> ...   # ESP32
      arm-none-eabi-objdump   -s --start-address=<sym> ...   # Pico
- libopus and the WDL resampler as IDF components, reusing the same submodules
  the Pico build compiles.

### Gotchas already worked around

Each of these silently does the wrong thing rather than failing loudly:

- BTstack's `port/esp32` `btstack_config.h` only defines `ENABLE_CLASSIC` under
  `CONFIG_IDF_TARGET_ESP32`, and its component gates `src/classic` the same way
  — both written when the original ESP32 was the only BR/EDR part. Left alone,
  BR/EDR quietly vanishes on S31.
- `btstack_port_esp32.c` sizes its HCI ring buffer with Bluedroid's `HCI_HOST_*`
  macros, which no longer exist in IDF 6.2 under `CONFIG_BT_CONTROLLER_ONLY`.
- `espressif/esp_tinyusb` ships a `tusb_config.h` on a public include path that
  shadows the firmware's and disables the device stack, and defines descriptor
  callbacks that clash with `usb_descriptors.cpp`. Use raw `espressif/tinyusb`.
- `CONFIG_BT_CLASSIC_ENABLED` is a Bluedroid **host** symbol and does nothing
  here; `CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY` is what sets `BT_CTRL_BREDR_ENABLE`.
- TinyUSB calls back into `main` for descriptors with nothing pointing that way
  in the dependency graph, so `main` needs `WHOLE_ARCHIVE`.
- **A DualSense that reconnects without rebooting keeps its own lightbar.** After
  a PS-hold that follows a fresh pairing, or when the dongle resets under a running
  controller, the controller keeps its blue connect pulse and flashing player LEDs
  and ignores every colour the host sends -- including after a `ResetLights` pulse
  (`src/utils.h` bit 1.3), which is what SDL sends. Rumble, audio and input all
  work, so only the lights show it. DS4Windows (`DualSenseDevice.cs`) documents
  the mechanism: a lightbar fade-out is "scheduled after fade-in completion", and
  "only a fade-out can cancel the pulse". So on every link `bt_task()` sends a
  fade-in on its own, then on the next loop pass a fade-out carrying the host's
  last colour and player lights, then a plain repaint 150 ms later. It starts on
  the first full (0x31) input report once its sensor timestamp passes SDL's
  10200000 connect-animation gate, or 3 s after the channel opens. Verified on a
  fresh pair, a PS-hold reconnect (stuck on every earlier attempt) and a
  power-off; waits of 3 s, 1 s and none between the two reports all worked, because
  the controller queues the fade-out itself. Never send the fade-in without the
  fade-out: a fade-in followed by anything else locks the bar on blue. Not applied
  on RP2350, which was never tested with a reconnect that skips the reboot; if it
  shows the same stuck lightbar, this sequence is the fix.

### High speed

This target enumerates at high speed. So does the real DualSense (confirmed
from its descriptors on hardware), and the S31's only PHY is high-speed, so
this is both the faithful choice and the natural one -- the RP2350 is
full-speed only because it has no alternative.

Four things had to be right together. Each fails quietly on its own, which is
why partial attempts looked like dead ends:

- **`bInterval` encoding.** A frame count at full speed, an exponent at high
  speed, so a literal 1 asks for 125 us rather than 1 ms. The audio OUT
  endpoint (`wMaxPacketSize` 392 -- one 1 ms frame of 48 kHz 4-channel 16-bit)
  then described a device that cannot exist: the host took a single
  isochronous OUT packet and abandoned the stream. `ep_interval()` in
  `src/usb_descriptors.cpp` re-encodes per speed.

- **Isochronous handling in dwc2.** `EPCTL_SD0PID_SEVNFRM` is SetEvenFrame on
  an isochronous endpoint, not a data toggle; and the target (micro)frame is
  one *interval* away, not the next one. Those coincide only at interval 1, so
  both defects are invisible at full speed and fatal at high speed, where
  `bInterval` 4 means 8 microframes. See `patches/tinyusb-dwc2-esp32s31.patch`.

- **USB interrupt priority and placement.** `esp_intr_alloc` was called with
  `ESP_INTR_FLAG_LOWMED` -- the lowest free level -- for an ISR with a 1 ms
  deadline, and `dcd_int_handler` sat in flash while the rest of the hot path
  had been moved to RAM. Now LEVEL3, and in `main/ds5_hot_iram.lf`.

- **Bluetooth controller core.** `esp_intr_alloc` registers the USB interrupt
  on whichever core calls it, which is the main task on core 0. With the BT
  controller also on core 0, its interrupt-disabling critical sections stalled
  the isochronous endpoint for milliseconds at a time -- about 4% of packets
  lost, heard as static. It leaves the main loop untouched, so loop-level
  instrumentation cannot see it. `CONFIG_BT_CTRL_PINNED_TO_CORE=1`.

The microphone needed one more, on its own. **Isochronous IN parity at high
speed:** dwc2 raises an incomplete-ISO-IN interrupt for each matching-parity
microframe the host did not poll -- several times between polls at `bInterval` 4
-- and the stock driver treated every one as a miss, flipping the parity and
eventually abandoning the transfer. Measured with the patch's counters: 745
packets/s sent and 85/s abandoned, heard as garble. The patch records each
transfer's target (micro)frame, ignores flags raised before it, flips the parity
only on a real miss, and locks it once a transfer completes: 1000 packets/s, clean
recordings. Build with `-DDS5_USB_ISO_STATS=ON` to print the counters once a
second (`main/CMakeLists.txt`).

### CPU budget

Core 1 runs the audio worker: Opus encode for the speaker, Opus decode for the
microphone. The S31's FPU is single-precision only, while CELT's float paths
(`celt_sqrt`, `celt_exp` and the rest of `mathops.h`) call double-precision libm,
which this core runs in software. `components/libopus/CMakeLists.txt`
force-includes `opus_single_precision.h`, which maps `sqrt`, `cos`, `exp` and the
others to their float versions, and adds `-fsingle-precision-constant`. Both apply
to the CELT sources and `opus{,_encoder,_decoder}.c` only. It also turns on
upstream's `OPUS_FLOAT_APPROX`. The CELT hot functions and the WDL resampler run
from RAM via `main/ds5_hot_iram.lf`. The clock is IDF's 320 MHz default for this
part (`sdkconfig.defaults`).

Measured: a 10 ms decode went from about 2.7 ms to under 1 ms. At 320 MHz the
boot bench (100 runs, 71-byte packet) reports decode 718 us average and encode
1362 us. A live stream encodes at about 1.6-1.7 ms per frame, roughly a quarter
of core 1.

Full speed is possible here but is not what ships, and is recorded because the
obvious way to ask for it bricks the bus. `DCFG.DevSpd = DCFG_DSPD_FS_HSPHY`
signals full speed over the high-speed PHY; what does not work is asking
TinyUSB. `dwc2_core_is_highspeed_phy()` decides from `GHWCFG2.fs_phy_type`, an
RTL parameter describing what the core supports rather than what the SoC bonds
out: the S31 reports 2 ("FS pins shared with UTMI+ pins") while `soc_caps.h`
says `SOC_USB_FSLS_PHY_NUM 0` and `gpio_sig_map.h` routes no USB OTG signals
at all. So `BOARD_TUD_MAX_SPEED=OPT_MODE_FULL_SPEED` runs `phy_fs_init()`,
sets `GUSBCFG.PHYSEL=1`, points the core at a transceiver that does not exist,
and `reset_core()` latches it -- a silent bus, nothing enumerated. Writing
`DCFG.DevSpd` afterwards cannot undo it. Espressif's own `esp_tinyusb`
hardcodes high speed for this part and states the port "is always HS".

`usb_log_speed()` prints `DCFG.DevSpd` and `DSTS.enum_speed` at boot. DevSpd is
what was requested; DSTS is what the link negotiated, and only the second is
evidence.

### Threading model

Worth understanding before changing anything here.

BTstack's own `port/esp32` hardwires `btstack_run_loop_freertos`, whose
`execute()` never returns -- it owns the calling task. `main.cpp` is a superloop
that must keep servicing TinyUSB, so it cannot hand its task over. Running
BTstack on a separate task instead would move every `bt.cpp` callback off the
USB thread, and the shared code is not written for that: `bt_write()` uses a
single static `send_element` and the report sequence counters are non-atomic,
safe only because the Pico ran them strictly sequentially on core 0.

So this port mirrors the Pico's cooperative arrangement instead: the pollable
`btstack_run_loop_embedded`, pumped from `port::bt_transport_poll()` exactly
where the Pico build called `cyw43_arch_poll()`. Every BTstack callback runs on
the main task.

The VHCI callbacks are the one genuinely cross-thread part, since the controller
invokes them on its own task. They only write to a mutex-guarded ring buffer
that the main task drains. (BTstack's port defers via
`btstack_run_loop_execute_on_main_thread()`, which on the embedded run loop is
an unlocked list insert -- correct for its FreeRTOS loop, a data race here.)

**Scope of that claim, precisely.** It means BTstack callbacks are on one task.
It does NOT mean the firmware is single-threaded, and an audit caught the
original wording implying that. Two other contexts run firmware code:
`port::timer_once_ms()` dispatches on the **esp_timer task** (priority 22, core
0) against a main task at priority 1 -- so `status_gpio.cpp`'s callback really
does preempt -- and the core-1 audio worker is a real task on a real second
core.

Core allocation follows from the same reasoning. The audio worker's loop never
blocks, because on RP2350 it owns a bare core. Core 1 is therefore given to it,
and `esp_timer` and the main task are pinned to core 0 explicitly in
`sdkconfig.defaults` rather than left to defaults -- a default flipping to core 1
would surface as audio glitching under load. The one exception is the BT
controller, pinned to core 1 (`CONFIG_BT_CTRL_PINNED_TO_CORE_CHOICE_1`): on core 0
its interrupt-disabling critical sections stalled the USB isochronous endpoint
(see "High speed"), and the worker's Opus load leaves it room on core 1 (see
"CPU budget").

### Deliberately accepted, with tripwires

Audited and judged safe *as the code stands*. Each has a condition that would
make it unsafe -- check these before changing the relevant area.

- **TinyUSB's audio class driver runs in the dwc2 ISR** (`audiod_xfer_isr`,
  `audiod_sof_isr`), unlike HID. Safe only because no firmware code sits on that
  call graph and tu_fifo is SPSC-correct with the main task as sole producer of
  `ep_in_ff` / sole consumer of `ep_out_ff`.
  **Tripwire:** implementing any `tud_audio_*_done_*` callback, adding a second
  `tud_audio_write` site, or calling `tud_audio_clear_ep_in_ff` puts firmware
  state under interrupt preemption -- and `audiod_init` never calls
  `tu_fifo_config_mutex`, so the FIFO lock is a no-op.
- **The core-1 worker spins at priority 23 and never yields**, so core 1's idle
  task never runs. Deliberate. The only thing that must still preempt it is the
  IPC task used by the flash cache-disable path, at priority 24 -- one level
  above.
  **Tripwire:** any task pinned to core 1 below priority 23 will silently never
  run.
- **The HCI RX ring drops rather than applying back-pressure** (no
  `ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL`). An earlier estimate here, that
  the ring held about 350 ms of traffic, was wrong. It overflowed on every connect
  while `usb_reconnect()` slept 150 ms inside the Bluetooth handler: the main loop
  was away about 250 ms and about 50 inbound ACL packets were dropped (no HCI
  events). `usb_reconnect()` now finishes from the main loop on this target (see
  the latent-defect entry below). The measured connect stall is 90-125 ms and
  nothing drops. Note `kAclPacketNum = 8` is worst-case *byte* sizing, not a slot
  count. Drops are counted, split into HCI events and ACL, and reported from
  `bt_transport_poll()`.
  **Tripwire:** anything that blocks the main loop for longer, especially from
  a Bluetooth callback, will start dropping packets again.
- **The hot path runs from RAM, at the cost of DIRAM that is shared with the
  heap.** `PORT_FAST_FUNC` maps to `IRAM_ATTR` and covers firmware code
  (`audio_loop`, `speaker_proc`, `mic_proc`, `interrupt_loop`, both packet
  handlers). Submodule code cannot take the attribute, so `main/ds5_hot_iram.lf`
  maps it instead -- the ESP-IDF counterpart of the RP2350 build's Phase B.2-B.4
  objcopy renames, same function list, first-class mechanism. Verified in the map:
  btstack's `packet_handler`, `hci_run`, `l2cap_acl_handler`, `l2cap_run`, the
  send paths and list iterators, plus TinyUSB's `tud_task_ext`, `tu_fifo_*`,
  `dcd_edpt_xfer`, `audiod_xfer_isr` and the HID report tail all sit in DIRAM
  (0x2F000000-0x2F080000). The fragment also maps the Opus CELT hot functions and
  the WDL resampler (see "CPU budget"). With all of it in place, `idf.py size`
  reports DIRAM 69% used, 154 KB free. In a live audio stream the internal heap
  measured 89 KB free, 57 KB at its low point.
  **Tripwire:** DIRAM is shared with the heap, so every function added to that
  fragment comes out of the allocator. It is scoped to the audio/BT per-packet
  path on purpose; `state_update` and the rest of the host->controller rumble/LED
  path stay in flash, exactly as on the Pico.
- **A flash erase or write disables the cache for both cores**, so anything still
  at 0x4001xxxx stalls for its duration -- `bt_task`, `config_save`, most of the
  BT host outside the relocated set. The audio and per-packet paths survive
  because they are in RAM. `SOC_SPI_MEM_SUPPORT_AUTO_SUSPEND` is 1 on this chip
  but `CONFIG_SPI_FLASH_AUTO_SUSPEND` is off, so nothing shortens that stall
  today.
  **Tripwire:** if a config save from the web UI, or a blacklist persist, ever
  glitches audio, the levers in order are that Kconfig option, then registering
  the USB interrupt with `ESP_INTR_FLAG_IRAM` (which additionally requires every
  callee on the ISR path to be IRAM-resident), then widening the fragment.

### Latent defects this port surfaced, deliberately NOT changed on RP2350

Several defects here were never ESP32 problems. The firmware was doing the wrong
thing already; the S31 is simply less forgiving than the CYW43439 and turned each
one into a visible failure.

**None of these are fixed on RP2350, and that is deliberate.** That target works
today, has years of field use behind it, and its current shape may encode
workarounds for problems earlier maintainers hit and did not write down.
Changing it to satisfy a defect that only manifests on different silicon trades
a known-good target for a theoretical improvement. Each is guarded to ESP32-S31
and recorded here so the reasoning survives -- not as a work list.

If one of these is ever pursued upstream, the notes below include what to watch
for, and in the `l2cap_send()` case an explicit trap to avoid.

Should anyone revisit that decision, test each in isolation, and after every one:
fresh PS+Share pair, PS-button reconnect, USB enumeration, speaker, mic.

- **Duplicate HCI commands** (`src/bt.cpp`, guards around the link key
  reply in both branches, user confirmation, `hci_set_connection_encryption`,
  and `hci_accept_connection_request`). BTstack already sends all of these from
  `hci_run()`; the application sent them again. CYW43439 rejects the second with
  `0x0C` Command Disallowed and continues, so the fault is invisible there. The
  ESP32-S31 controller asserted in `olm_lmp_ssp.c:1820` and reset. Highest
  priority of these: it is the same class of defect that crashed one controller
  outright, and there is no reason to assume that controller is the last one
  this firmware meets. Watch for: pairing completing normally, and no change to
  reconnect.
- **Page scan violates the interlaced-scan rule** (`src/bt.cpp`,
  `gap_set_page_scan_activity(0x0012, 0x0012)` alongside
  `PAGE_SCAN_MODE_INTERLACED`). Core Spec 5.4, Vol 2 Part B 8.3.1: "If the scan
  interval is not at least twice the scan window, then generalized interlaced
  scan shall not be used" -- an interlaced scan is two back-to-back windows, so
  equal interval and window leaves the second nowhere to go. Neither HCI command
  can reject it, so both return success and the controller is silently
  misconfigured. Equal values are also R0, a 100% duty cycle, which the same
  section warns starves anything sharing the radio -- including the LMP exchanges
  of a link still forming. Changed to 0x50/0x12 on ESP32-S31 only, matching what
  BlueRetro ships; Linux uses 0x0100/0x0012 for interlaced and the Bluetooth
  default is 0x0800/0x0012. Watch for: reconnect latency, since this lowers page
  scan duty from 100% to 22%.

  On ESP32-S31 this is the change that made reconnect-after-PS-hold reliable,
  confirmed by reverting the other candidate independently (below). Duty cycle
  went DOWN and reliability went UP, which is the tell that the 100% scan was
  starving the LMP exchange of the link it was supposed to be accepting.
- **`l2cap_send()` status discarded** (`control_send()` in `src/bt.cpp`).
  It returns `BUFFERS_FULL` when the controller has no free ACL buffer, and the
  return value is dropped. `init_feature()` issues four requests back to back,
  so on the ESP32-S31 three vanished silently -- including 0x05 calibration and
  0x20 firmware version. CYW43439 has buffers enough that the burst fits, so the
  bug is latent rather than absent.

  Do NOT simply un-guard the ESP32 fix. Deferring these sends into a FIFO is
  what broke the RP2350 once already: the 0x20 reply drives the
  "Connected DS5 Controller" branch, the only caller of `tud_connect()`, so the
  gamepad paired and the USB device never appeared. If this is fixed for both,
  fix it by checking the status and retrying, not by deferring the send.
- **`update_state()` sends a constant sequence number** (`pkt[1] = 0x10`), while
  every other output report takes the link's 4-bit counter. On ESP32-S31 it takes
  the next counter value too. This caused no visible fault on either target; it is
  recorded because a constant is wrong whenever audio reports are flowing.
- **`usb_reconnect()` blocks for 150 ms**, and runs from the L2CAP handler on the
  0x20 feature reply. On ESP32-S31 that stalled the Bluetooth pump long enough to
  overflow the VHCI ring on every connect. There it now disconnects and returns,
  and `usb_reconnect_task()` in the main loop reconnects 150 ms later. The
  CYW43439 holds traffic back instead of dropping it, so the Pico keeps the delay.

### Investigated and reverted

- **Disabling inquiry scan.** `gap_discoverable_control(0)` was set on
  ESP32-S31, reasoning that a bonded controller pages us rather than discovering
  us, and that neither Bluepad32 nor BlueRetro enable inquiry scan. It went in
  alongside the page scan change while reconnect was broken, and reconnect
  started working, so it looked load-bearing. It was not: reverting it alone,
  with the page scan fix kept, left reconnect just as reliable. That part is
  settled and visible in any reconnect log.

  **The lightbar conclusion drawn from the same test was wrong.** That build
  also had `DS5_FEATURE_TRACE=1`, whose blocking `printf`s sit on the output
  report path, so the lightbar working was not attributable to the inquiry scan
  change it was meant to test. With the trace off the symptom returned. See the
  lightbar entry above: it is the controller's own behaviour, and `ResetLights`
  turned out not to be the fix at all.

  Three lessons, all paid for in hardware test cycles:

  - Changing two radio settings in one build to fix one symptom means neither is
    attributed, and the unattributed one is free to cause the next bug.
  - A test only isolates the variable you changed *deliberately*. Diagnostic
    output is a variable: it costs milliseconds on the path being measured, and
    it silently differed between the build that passed and the build that
    shipped.
  - Verify the shipping configuration, not a near neighbour of it. Confirming
    with tracing on and then shipping with it off is not a verification.

- **Lightbar theories ruled out on hardware** before the fade-out finding. Each
  was in a build tested against the stuck reconnect, and none changed it:
  `ResetLights` content and timing (SDL's exact bytes and gate), holding audio
  reports back until the lights were released, report 0x31 against 0x32, brightness
  or "none" lightbar-setup bits, the sequence number, and the dropped HCI packets.
  In this condition the controller's own input reports look the same as on a link
  that paints, and its sensor timestamp carries on from the previous link instead
  of restarting.

### Not done

- ~~Preemption assumptions need auditing.~~ Done. A four-lens adversarial audit
  confirmed the BTstack-callbacks-on-one-task claim by tracing all four
  `bt_write()` callers, and found five real defects, all fixed: a timer
  use-after-free, blocking `printf` on two must-not-block contexts, a
  two-consumer race on `audio_spk_fifo`, a non-atomic `mic_active`, and a
  `CFG_TUSB_OS` mismatch between the app and the TinyUSB library. One
  behavioural change fell out: `audio_spk_fifo` overflow now drops the newest
  frame rather than the oldest, on both targets.
- **The status GPIO is not configurable here, and the pin model is not
  target-aware anywhere.** `status_gpio_pin_valid()` excludes UART, VSYS/VBUS,
  SMPS and CYW43 pins through `#ifdef PICO_*` guards, all of which compile out
  on this target, so the ESP32 accepts any pin below `SOC_GPIO_PIN_COUNT` --
  including ones the board already uses (status LED 60, BOOT button 61) and
  strapping pins. The web config compounds it: its selector offers Pico 2 W
  GPIOs and rejects anything else, so a stored pin of 0 is reported back as
  invalid and the page refuses the config.

  This is not specific to the ESP32. `waveshare_rp2350b_plus_w` is RP2350**B**
  with 48 GPIOs against the Pico 2 W's 30 (`PICO_RP2350A 0` in its board
  header), so that target already accepts pins 30-47 the web config will never
  offer, with its reserved pins at different numbers. One hardcoded pin list is
  serving three different maps.

  A real fix is three parts: the firmware reports its target (a new report
  alongside `0xf8`/`0xf9` is cheaper than a `Config_body` field, which would
  mean a `CONFIG_VERSION` bump and a migration), the web config carries a pin
  map per target, and `status_gpio_pin_valid()` gets a port hook instead of
  more `#ifdef`s. Part of that lives in the web config repo, so it is not
  self-contained here.

  Deliberately deferred. The Alef is dongle-style, but the header is still not
  why anyone would pick it, and the fix spans the web config repo. Worth doing
  once someone actually wires a status GPIO on an S31 board -- at which point
  the Waveshare target needs it too.
- **`reboot_to_bootloader()` falls back to a plain restart.** For S31 this is
  `LP_SYSTEM_REG_SYS_CTRL_REG` bit 2 `FORCE_DOWNLOAD_BOOT` (P4-style LP_SYSTEM,
  not the S3's `RTC_CNTL`) followed by a reset. Unverified — confirm against the
  S31 TRM before trusting it.
- `ram_mem.c`'s memcpy/memset overrides are carried over unchanged; whether they
  still pay for themselves against ESP32-S31 IRAM placement is untested.
