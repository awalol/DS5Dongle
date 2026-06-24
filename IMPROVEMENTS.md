# Millores / full de ruta — DS5Dongle WoL

Estat: **release de rendiment** (2026-06-24) construïda i flashejada; **validació de
maquinari pendent** (vegeu §0). L'àudio ja corre des de RAM (camí CELT-encode) sense
sacrificar el WoL. Vegeu [CONTEXT.md](CONTEXT.md) §3.4.

---

## 0. ⚠️ PRIMER: matriu de verificació de maquinari d'aquesta release

El build està verificat al mapa (CELT-encode a RAM, SILK a flash, ~276 KB heap
lliure) però **encara no s'ha provat amb el comandament**. Cal passar TOTA aquesta
matriu abans de considerar-la estable. Rollback si alguna cosa falla: posa la Pico en
BOOTSEL i `Copy-Item build-baseline\ds5-bridge.uf2 E:\` (firmware anterior que funcionava).

1. **Emparellament:** PS+Share amb un DualSense → connecta, els inputs van.
2. **Reconnexió:** apaga/encén el comandament → es reconnecta sol.
3. **BOOTSEL:** clic = nova inquiry; mantenir = esborra pairings (triple flash) → re-pair OK.
4. **Wake-on-PS:** PC en repòs (S3) → PS → el PC desperta.
5. **WoL en fred:** PC apagat (S5), Pico alimentat → PS → el PC s'encén i el
   comandament **es manté connectat** tota l'arrencada (un sol PS). Repetir des de fred.
6. **Àudio altaveu:** reprodueix àudio del PC → net, **sense talls** (això valida que
   la relocació CELT ha eliminat els XIP-stalls).
7. **Àudio mic:** obre el mic del comandament (app que gravi) → veu neta; obrir/tancar
   repetidament → sense corrupció. (Decode CELT ara a RAM, P3 ✅.)
8. **Haptics:** vibració/efectes → correctes.
9. **DSE Edge:** perfils DSE → es carreguen/commuten bé.
10. **Config save:** canvia un valor, desa, reinicia → persisteix; el watchdog (1 s) no salta.
11. **Estabilitat / OOM:** **≥10 cicles** pair/unpair **+ WoL** amb el WiFi amunt →
    sense `*** PANIC *** Out of memory`. (És el guardià del pressupost de RAM.)

Si 6/7/8 (àudio) milloren respecte abans i res de 1–5,9–11 es trenca → release OK.

---

## P1 — ✅ FET: àudio perfecte mantenint el WoL (relocació selectiva d'Opus)

Resolt a la release 2026-06-24. Es relloca a RAM **només** el camí CELT-encode
(18 TUs, ~87 KB) en lloc de tot `libopus` (~241 KB), perquè
`OPUS_APPLICATION_RESTRICTED_LOWDELAY` = CELT-only (SILK mai s'executa). Mecanisme a
[cmake/relocate_archive_members.cmake](cmake/relocate_archive_members.cmake), gated
sota `ENABLE_WOL` a [CMakeLists.txt](CMakeLists.txt). Heap lliure ~276 KB (2,3× per
sobre del nivell d'OOM). Vegeu [CONTEXT.md](CONTEXT.md) §3.4.

---

## P2 — Reclamar el stack de core1 (32 KB) — **el guany de RAM més gros que queda**

`audio_core1_stack` són **32 KB** (`uint32_t[8192]` a [audio.cpp](src/audio.cpp)),
molt probablement sobredimensionat per a un encoder CELT a complexity 0. Però
**reduir-lo a cegues pot corrompre `.bss` silenciosament** (no hi ha MPU guard), per
això cal **mesurar** primer. Procediment segur:

1. **Pintar el stack:** a `core1_entry`, just després de `flash_safe_execute_core_init()`,
   omplir `audio_core1_stack` amb un sentinella `0xA5A5A5A5`. Afegir una funció
   `audio_core1_stack_high_water_bytes()` que compti des del fons quants bytes ja no
   són el sentinella. Exposar-la (p. ex. via un `pico_cmd` 0xf6 a [cmd.cpp](src/cmd.cpp)
   llegit amb l'eina de config, o un `printf` amb `ENABLE_SERIAL`).
2. **Mètode estàtic (corroboració):** compilar amb `-fstack-usage` (target + `opus` +
   `wdl_resampler`) i sumar la cadena pitjor `core1_entry → speaker_proc →
   opus_encode_float → celt_encode_with_ec` i `mic_proc → opus_decode`, **+ marge**
   pels frames `alloca`/VLA de CELT i ≥256 B d'stacking FP/IRQ del Cortex-M33.
3. **Soak pitjor cas** ≥60 s: mic OBERT + àudio 48 k a l'altaveu + haptics + una
   reconnexió BT + un `config_save` (que aparca core1). Llegir el high-water.
4. **Fixar mida** = `arrodonir(pic_mesurat + 8 KB de marge)`. **Clamps de seguretat:**
   mai per sota de 7168 paraules (28 KB) sense doble mesura; mai sota 6144 (24 KB).
   Si no es mesura, **deixar 8192**.
5. Treure la instrumentació de pintat abans de la release final.

Estalvi potencial: ~12–24 KB de RAM. Risc: **alt** sense mesura.

---

## P3 — Més àudio/RAM a RAM

- ✅ **FET (2026-06-24): decode del mic a RAM (+17 KB).** Afegits `celt_decoder.c.obj`,
  `opus_decoder.c.obj`, `entdec.c.obj` a `OPUS_RAM_MEMBERS` a
  [CMakeLists.txt](CMakeLists.txt). El mic del DualSense és CELT-only (confirmat per
  anàlisi + oïda), així que aquests 3 TUs incrementals (la resta del camí CELT ja era
  a RAM per l'encode) fan el mic perfecte. Heap ~260 KB (segur). Verificat al mapa:
  `celt_decode_with_ec`/`opus_decode` a `0x2001xxxx`.
- **Taules `.rodata` d'Opus a RAM (+20 KB) — opcional, gated:** renombrar `.rodata` de
  `modes.c.obj` i `cwrs.c.obj` (taules MDCT/FFT/allocation llegides per frame). Apropa
  el heap al terra de 250 KB (260 − 20 = 240 < 250) → **NO fer sense alliberar RAM
  primer** (P2 stack o P4 lwIP). Només si un guardià de maquinari (≥10 cicles pair+WoL
  sense PANIC) passa amb marge.

---

## P4 — lwIP més petit (~9 KB) — RAM-freeing, risc mitjà al WoL

Allibera RAM **abans** de gastar-ne més (p. ex. si es fa P3). Dues opcions amb el
mateix estalvi (~9 KB de `PBUF_POOL` + `ram_heap`):

- **IP estàtica** (`LWIP_DHCP=0` + `PBUF_POOL_SIZE` 8→6 + `PBUF_POOL_BUFSIZE` ~600 +
  `MEM_SIZE` ~1600): més robust per al WoL, però l'usuari ha de triar una IP/gateway/
  màscara lliures de la xarxa (afegir placeholders a `secrets.h.example`, **mai valors
  reals**). Cal validar que el netif arriba a link-up amb la IP estàtica i el magic
  packet surt.
- **Híbrid (manté DHCP):** `MEM_SIZE` ~2600 + `PBUF_POOL_SIZE` 6 + `BUFSIZE` 768 +
  `IP_SOF_BROADCAST_RECV` 0. Sense càrrega de config per a l'usuari.

⚠️ Validar: DHCP/associació completa, magic packet desperta el PC, i ≥10 cicles
pair+WoL sense OOM. **No és necessari per seguretat** (P1 ja deixa ~276 KB); és
headroom extra.

---

## P5 — Qualitat de vida / upstream

- Els fixos generals (gating `!tud_suspended()`, `wake_suppress_poweroff`, i la
  relocació selectiva CELT) es poden proposar a l'upstream `awalol/DS5Dongle` via PR.
- `WOL_UDP_LOG` / `WOL_FORCE_TEST` són només DEBUG (OFF per defecte).
- (Opcional) `--specs=nano.specs` per a un newlib més petit (uns KB de `.bss/.data`),
  només link-time i amb regressió completa de pairing (canvia el `malloc`).

---

## Notes per reprendre ràpid

- Producció: `cmake -S . -B build ... -DENABLE_WOL=ON` + `ninja -C build ds5-bridge`.
- **Build net obligatori** si canvies `ENABLE_WOL` o la llista de membres d'Opus
  (l'objcopy/ar muta `libopus.a` in-place).
- Verificar la relocació al binari final: `arm-none-eabi-nm build\ds5-bridge.elf`
  → els símbols CELT (`celt_encode_with_ec`…) han de ser a `0x2001xxxx` (RAM) i els
  `silk_*` a `0x10xxxxxx` (flash).
- `src/secrets.h` és local i gitignored; **mai** a git.
