# Improvements / Roadmap — DS5Dongle WoL

Status: **performance release** (2026-06-24), built and flashed; **hardware validation
pending** (see §0). Audio now runs from RAM (the CELT-encode path) without giving up
WoL. See [CONTEXT.md](CONTEXT.md) §3.4.

---

## 0. ⚠️ FIRST: hardware verification matrix for this release

The build checks out in the map file (CELT-encode in RAM, SILK in flash, ~276 KB free
heap), but it **has not yet been tested with a controller**. The entire matrix below
must pass before the release can be considered stable. To roll back if something
breaks, put the Pico into BOOTSEL and run `Copy-Item build-baseline\ds5-bridge.uf2 E:\`
(the previous known-good firmware).

1. **Pairing:** PS+Share with a DualSense → connects, inputs work.
2. **Reconnect:** power the controller off/on → it reconnects on its own.
3. **BOOTSEL:** tap = new inquiry; hold = clear pairings (triple flash) → re-pair OK.
4. **Wake-on-PS:** PC asleep (S3) → PS → the PC wakes.
5. **Cold WoL:** PC off (S5), Pico powered → PS → the PC powers on and the controller
   **stays connected** through the whole boot (a single PS press). Repeat from cold.
6. **Speaker audio:** play audio from the PC → clean, **no dropouts** (this confirms the
   CELT relocation eliminated the XIP stalls).
7. **Mic audio:** open the controller mic (any recording app) → clean voice; open/close
   repeatedly → no corruption. (CELT decode now runs in RAM, P3 ✅.)
8. **Haptics:** rumble/effects → correct.
9. **DSE Edge:** DSE profiles → load and switch correctly.
10. **Config save:** change a value, save, reboot → it persists; the watchdog (1 s)
    does not trip.
11. **Stability / OOM:** **≥10 cycles** of pair/unpair **+ WoL** with WiFi up → no
    `*** PANIC *** Out of memory`. (This is the guard on the RAM budget.)

If 6/7/8 (audio) improve over the previous build and nothing in 1–5, 9–11 breaks → the
release is good.

---

## P1 — ✅ DONE: clean audio while keeping WoL (selective Opus relocation)

Fixed in the 2026-06-24 release. Only the CELT-encode path is relocated to RAM
(18 TUs, ~87 KB) instead of all of `libopus` (~241 KB), because
`OPUS_APPLICATION_RESTRICTED_LOWDELAY` is CELT-only (SILK never runs). The mechanism
lives in [cmake/relocate_archive_members.cmake](cmake/relocate_archive_members.cmake),
gated behind `ENABLE_WOL` in [CMakeLists.txt](CMakeLists.txt). Free heap is ~276 KB
(2.3× above the OOM threshold). See [CONTEXT.md](CONTEXT.md) §3.4.

---

## P2 — Reclaim the core1 stack (32 KB) — **the largest remaining RAM win**

`audio_core1_stack` is **32 KB** (`uint32_t[8192]` in [audio.cpp](src/audio.cpp)),
almost certainly oversized for a CELT encoder running at complexity 0. But **shrinking
it blindly can silently corrupt `.bss`** (there is no MPU guard), so it has to be
**measured** first. Safe procedure:

1. **Paint the stack:** in `core1_entry`, right after
   `flash_safe_execute_core_init()`, fill `audio_core1_stack` with an `0xA5A5A5A5`
   sentinel. Add an `audio_core1_stack_high_water_bytes()` function that counts, from
   the bottom up, how many bytes are no longer the sentinel. Expose it (e.g. via a
   `pico_cmd` 0xf6 in [cmd.cpp](src/cmd.cpp) read by the config tool, or a `printf`
   under `ENABLE_SERIAL`).
2. **Static method (cross-check):** compile with `-fstack-usage` (target + `opus` +
   `wdl_resampler`) and sum the worst-case chain `core1_entry → speaker_proc →
   opus_encode_float → celt_encode_with_ec` and `mic_proc → opus_decode`, **plus
   margin** for CELT's `alloca`/VLA frames and ≥256 B of Cortex-M33 FP/IRQ stacking.
3. **Worst-case soak** ≥60 s: mic OPEN + 48 k audio to the speaker + haptics + one BT
   reconnect + one `config_save` (which parks core1). Read the high-water mark.
4. **Set the size** = `round(measured_peak + 8 KB margin)`. **Safety clamps:** never
   below 7168 words (28 KB) without a second measurement; never below 6144 (24 KB). If
   not measured, **leave it at 8192**.
5. Remove the paint instrumentation before the final release.

Potential saving: ~12–24 KB of RAM. Risk: **high** without measurement.

---

## P3 — More audio/RAM into RAM

- ✅ **DONE (2026-06-24): mic decode in RAM (+17 KB).** Added `celt_decoder.c.obj`,
  `opus_decoder.c.obj`, and `entdec.c.obj` to `OPUS_RAM_MEMBERS` in
  [CMakeLists.txt](CMakeLists.txt). The DualSense mic is CELT-only (confirmed by
  analysis and by ear), so these 3 incremental TUs (the rest of the CELT path was
  already in RAM from the encode) make the mic clean. Heap ~260 KB (safe). Confirmed
  in the map file: `celt_decode_with_ec`/`opus_decode` at `0x2001xxxx`.
- **Opus `.rodata` tables in RAM (+20 KB) — optional, gated:** rename the `.rodata` of
  `modes.c.obj` and `cwrs.c.obj` (MDCT/FFT/allocation tables read per frame). This
  pushes the heap toward the 250 KB floor (260 − 20 = 240 < 250) → **do not do this
  without freeing RAM first** (P2 stack or P4 lwIP). Only worth it if a hardware guard
  (≥10 pair+WoL cycles with no PANIC) passes with margin.

---

## P4 — Smaller lwIP (~9 KB) — RAM-freeing, medium risk to WoL

Free RAM **before** spending more of it (e.g. if you take on P3). Two options with the
same saving (~9 KB of `PBUF_POOL` + `ram_heap`):

- **Static IP** (`LWIP_DHCP=0` + `PBUF_POOL_SIZE` 8→6 + `PBUF_POOL_BUFSIZE` ~600 +
  `MEM_SIZE` ~1600): more robust for WoL, but the user has to pick a free
  IP/gateway/mask for their network (add placeholders to `secrets.h.example`, **never
  real values**). Verify that the netif reaches link-up with the static IP and that the
  magic packet goes out.
- **Hybrid (keeps DHCP):** `MEM_SIZE` ~2600 + `PBUF_POOL_SIZE` 6 + `BUFSIZE` 768 +
  `IP_SOF_BROADCAST_RECV` 0. No config burden on the user.

⚠️ Validate: full DHCP/association, magic packet wakes the PC, and ≥10 pair+WoL cycles
with no OOM. **Not required for safety** (P1 already leaves ~276 KB); this is extra
headroom.

---

## P5 — Quality of life / upstream

- The general fixes (gating on `!tud_suspended()`, `wake_suppress_poweroff`, and the
  selective CELT relocation) could be proposed upstream to `awalol/DS5Dongle` via PR.
- `WOL_UDP_LOG` / `WOL_FORCE_TEST` are DEBUG-only (OFF by default).
- (Optional) `--specs=nano.specs` for a smaller newlib (a few KB of `.bss`/`.data`),
  link-time only and with a full pairing regression pass (it changes `malloc`).

---

## Notes for picking this back up quickly

- Production: `cmake -S . -B build ... -DENABLE_WOL=ON` + `ninja -C build ds5-bridge`.
- **A clean build is mandatory** if you change `ENABLE_WOL` or the Opus member list
  (objcopy/ar mutates `libopus.a` in place).
- Verify the relocation in the final binary: `arm-none-eabi-nm build\ds5-bridge.elf`
  → the CELT symbols (`celt_encode_with_ec`…) must be at `0x2001xxxx` (RAM) and the
  `silk_*` symbols at `0x10xxxxxx` (flash).
- `src/secrets.h` is local and gitignored; **never** commit it.