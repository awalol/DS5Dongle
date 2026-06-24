# Context del projecte — Pico 2W DualSense Bridge (DS5Dongle) — **v0.7.2-hotfix + Wake-on-LAN**

> Estat: **funcional i validat en maquinari real** (PC objectiu amb RTX 5090).
> Per compilar i flashejar, vegeu [BUILD.md](BUILD.md). Per a la propera tanda de
> millores, vegeu [IMPROVEMENTS.md](IMPROVEMENTS.md).

---

## 1. Què és el projecte

Firmware d'una **Raspberry Pi Pico 2W** (RP2350 + xip wireless CYW43) que la
converteix en un **dongle USB sense fils** per a comandaments **Sony DualSense
(DS5)** i **DualSense Edge (DSE)**. Fa de pont bidireccional:

```
   DualSense  ──(Bluetooth Classic, L2CAP/HID)──►  Pico 2W  ──(USB HID + Audio)──►  PC
```

- **Cap al comandament:** host Bluetooth Classic (BTstack) — inquiry, emparella,
  obre canals L2CAP HID (Control + Interrupt).
- **Cap al PC:** emula un DualSense real (VID/PID de Sony) amb interfície HID +
  USB Audio (UAC1), incloent vibració hàptica i àudio de l'altaveu/jack.
- **Doble nucli:** core0 = bucle principal (BT, USB, haptics, WoL); core1 =
  còdec d'àudio Opus.

---

## 2. La funció afegida: Wake-on-LAN (`ENABLE_WOL`, ON per defecte)

**Objectiu assolit (experiència "de consola"):** amb el PC **apagat (S5)** però el
Pico alimentat (USB standby o hub/extensor alimentat), en prémer **PS un sol cop**
el comandament connecta amb la Pico, aquesta envia un **magic packet de WoL** per
WiFi, el PC s'encén i el comandament **es manté connectat durant tota l'arrencada**
(sense haver de prémer PS un segon cop). Un cop el PC està encès, el WiFi es tomba
i el comandament funciona com sempre.

### Fitxers afegits (`src/`)
- [wol.cpp](src/wol.cpp) / [wol.h](src/wol.h) — màquina d'estats no bloquejant
  (`Idle → Observe → Connecting → Backoff → Sending → Cleanup`).
- [lwipopts.h](src/lwipopts.h) — config lwIP (poll, UDP + DHCP + broadcast).
- `secrets.h` — credencials locals (SSID, contrasenya, **MAC del PC**, port).
  **Gitignored**; plantilla a [secrets.h.example](src/secrets.h.example).

### Ganxos a fitxers existents
- [CMakeLists.txt](CMakeLists.txt) — opció `ENABLE_WOL`; quan ON: `CYW43_LWIP=1`,
  enllaça `pico_cyw43_arch_lwip_poll`, compila `wol.cpp`, **i NO relloca Opus a
  RAM** (vegeu §3.1).
- [main.cpp](src/main.cpp) — `wol_init()` després de `state_init()`; `wol_tick()`
  al final del bucle.
- [bt.cpp](src/bt.cpp) — `wol_request()` en obrir-se el canal L2CAP HID Interrupt.
- [wake.cpp](src/wake.cpp) / [wake.h](src/wake.h) — nova funció
  `wake_suppress_poweroff()` (vegeu §3.3).

---

## 3. Els TRES problemes resolts aquesta sessió (causa exacta + fix)

### 3.1 `*** PANIC *** Out of memory` → es penjava en emparellar

- **Símptoma:** amb el build WoL (i fins i tot el build stock+serial), el Pico
  trobava el comandament però **es penjava durant l'SSP**; no acabava de connectar.
  El binari oficial precompilat **sí** funcionava.
- **Causa:** el firmware relloca **~280 KB de codi d'Opus a RAM** (`.time_critical`,
  per rendiment d'àudio). Això deixa el *heap* molt just. En afegir lwIP (+~18 KB)
  per al WoL, un `malloc` durant la inicialització BT fallava → `panic("Out of
  memory")`. (BTstack usa pools estàtics; l'OOM venia del heap compartit Opus/lwIP.)
- **Fix:** a [CMakeLists.txt](CMakeLists.txt), **quan `ENABLE_WOL` està actiu NO es
  relloca `libopus` a RAM** (es queda a flash/XIP). Allibera ~280 KB → de ~83 KB
  lliures es passa a **~362 KB lliures**. El build stock (sense WoL) manté Opus a
  RAM com l'original.
- ✅ **Compromís RESOLT (release de rendiment, §3.4):** ja no cal triar entre WoL i
  àudio. Es relloca a RAM **només el camí CELT d'encode** d'Opus (~87 KB), no els
  241 KB sencers → àudio perfecte **i** ~276 KB de heap lliure.
- **Nota de build:** l'`objcopy`/`ar` que renombra les seccions modifica `libopus.a`
  *in place*; cal **build net** (esborrar `build*/`) perquè el canvi tingui efecte.

### 3.4 Release de rendiment — àudio perfecte amb headroom de RAM *(2026-06-24)*

- **Idea clau:** **ambdues direccions corren en CELT-only** — l'encoder de l'altaveu
  usa `OPUS_APPLICATION_RESTRICTED_LOWDELAY`, i el mic del DualSense puja CELT-only
  48 kHz fullband 10 ms (el SILK no pot representar 48 kHz fullband). El codi SILK
  **mai** s'executa al camí d'àudio. Per tant es relloca a RAM **només** el subconjunt
  CELT encode+decode (21 TUs, ~104 KB de `.text`) en comptes de tot `libopus`
  (~241 KB). Això elimina els *XIP-miss* per frame a core1 tant a l'**encode**
  (altaveu/haptics) com al **decode** (mic) → àudio perfecte als dos sentits, deixant
  ~260 KB de heap lliure — **>100 KB** per sobre del nivell que provocava l'OOM (§3.1).
- **Mecanisme:** [cmake/relocate_archive_members.cmake](cmake/relocate_archive_members.cmake)
  fa cirurgia per-membre a `libopus.a` (`ar x` → `objcopy --rename-section
  .text=.time_critical.opus_text` → `ar r` → `ar s`). A [CMakeLists.txt](CMakeLists.txt),
  la branca `ENABLE_WOL` ON fa la relocació **selectiva** (`OPUS_RAM_MEMBERS`); OFF
  manté la relocació de tot l'arxiu (comportament original). Verificat al mapa:
  `celt_encode_with_ec`, `celt_decode_with_ec`, `opus_decode`, `ec_dec_*`, etc. a
  `0x2001xxxx` (RAM); `silk_*` a `0x10xxxxxx` (flash).
- **Altres guanys d'aquesta release (risc zero, build-verificats):**
  - Pont de transport HCI btstack↔cyw43 (`hci_transport_data_source_process`,
    `..._send_packet`, `cyw43_bluetooth_hci_process`) rellocat a RAM → menys latència
    XIP al camí BT per-paquet que alimenta els informes d'àudio/haptics.
  - `ENABLE_LOG_INFO` de btstack tret (logging intern verbós; menys flash/CPU al
    camí d'esdeveniments BT). `ENABLE_PRINTF_HEXDUMP` es manté: el Pico SDK compila
    `hci_dump_embedded_stdout.c` que el requereix per `#error`, però es *gc-elimina*
    del binari final (hci_dump mai s'inicialitza).
  - `<iostream>`/`<iomanip>` + `print_hex()` mort trets de `utils.h`.
  - Barrera CMake: `FATAL_ERROR` si algú activa IPO/`-flto` (trencaria silenciosament
    tota la relocació `.time_critical`, que opera al PRE_LINK).
- **Micro-opts descartats després de verificar-los:** `/32768.0f`→recíproc ja el fa
  el compilador (`vdiv=0`); altres hoists eren a camins no-crítics o pessimitzaven el
  curtcircuit → es deixa el codi BT/àudio intacte.
- **Decode del mic (2026-06-24, 2a iteració):** afegit el conjunt CELT-decode
  (`celt_decoder` + `opus_decoder` + `entdec`, +17 KB) a `OPUS_RAM_MEMBERS` → mic
  perfecte també. `.data` 150 → 167 KB; heap ~276 → **~260 KB** (segur, >250 KB).
  Còdec del mic confirmat CELT per anàlisi (workflow) i validat per oïda.
- **Pendent (necessita mesura de maquinari, vegeu [IMPROVEMENTS.md](IMPROVEMENTS.md)):**
  reclamar el stack de core1 (32 KB), trim de lwIP (~9 KB), i opcionalment les taules
  `.rodata` d'Opus (+20 KB).

### 3.2 El WoL només es disparava el primer cop (gating del bus suspès)

- **Símptoma:** la 1a vegada (PC mai encès) el WoL s'enviava. La 2a (PC havia estat
  encès i després apagat) **no s'enviava**.
- **Causa:** el *gating* mirava només `tud_mounted()`. Quan el PC s'apaga però el
  port USB segueix donant corrent, el bus queda **suspès** (no desconnectat) i
  `tud_mounted()` segueix `true` → el WoL creia el PC encès i avortava.
- **Fix:** a [wol.cpp](src/wol.cpp), estat `Observe`, la condició d'avortar és ara
  **`tud_mounted() && !tud_suspended()`** (host *muntat I actiu*). Si està suspès
  (PC S5 amb standby) o desmuntat → es dispara el WoL. També es va pujar
  l'anti-rebot a **90 s** perquè les reconnexions durant el boot no re-disparin WoL.

### 3.3 Es perdia la connexió durant l'arrencada → calia un 2n PS *(fix definitiu)*

- **Símptoma:** el WoL despertava el PC, però **durant l'arrencada** el comandament
  s'apagava i calia prémer PS un segon cop. Passava també amb alimentació estable
  (extensor) → no era ni alimentació ni enumeració.
- **Diagnòstic clau:** logs capturats per WiFi (UDP) + observació de l'usuari (el LED
  del teclat USB i el del Pico parpellejaven al traspàs BIOS→Windows).
- **Causa exacta:** [wake.cpp](src/wake.cpp) té un estalvi de bateria que **apaga el
  comandament** (`bt_power_off_controller()`) si l'USB queda **suspès més de 3 s**
  (`WAKE_POWEROFF_DEBOUNCE_US`), interpretant-ho com "PC adormit/apagat". En una
  arrencada després del WoL l'USB està suspès molts segons mentre el PC arrenca →
  el firmware apagava el comandament a mig boot.
- **Fix (universal, no trenca la suspensió S3):** nova funció
  **`wake_suppress_poweroff(duration_us)`** a [wake.cpp](src/wake.cpp). El WoL la
  crida (a `wol_request()`, [wol.cpp](src/wol.cpp)) en connectar-se el comandament,
  suprimint l'apagat automàtic durant **180 s** (cobreix l'arrencada). Si el PC
  realment no arrenca, l'apagat es reactiva passat el termini (estalvi de bateria
  intacte). El comportament de suspensió S3 (apagar als 3 s + despertar amb PS) NO
  canvia.

---

## 4. Opcions de compilació (CMake)

| Opció | Defecte | Efecte |
|-------|---------|--------|
| `ENABLE_WOL` | **ON** | Wake-on-LAN; lwIP; **Opus CELT-encode a RAM** (selectiu, §3.4) |
| `ENABLE_SERIAL` | OFF | `printf` per USB CDC (canvia l'enumeració USB; sense watchdog) |
| `ENABLE_VERBOSE` | OFF | Logs BTstack detallats |
| `ENABLE_BATT_LED` | ON | LED de bateria baixa |
| `WOL_FORCE_TEST` | OFF | **DEBUG:** força el WoL en cada connexió (ignora el gating del PC) |
| `WOL_UDP_LOG` | OFF | **DEBUG:** redirigeix `printf` a UDP broadcast:9999 i manté el WiFi amunt |
| `PICO_W_BUILD` | OFF | Pico W (RP2040) |
| `WAKE_DEBUG` | OFF | Traça de la FSM de wake |

> Els dos `WOL_*` de DEBUG són eines de diagnòstic afegides aquesta sessió; en
> producció van **OFF**. `WOL_UDP_LOG` manté el WiFi actiu (estressa la coexistència
> BT) — només per capturar logs amb el PC objectiu apagat.

---

## 5. Estat validat (maquinari real)

- ✅ Emparellament net (resolt l'OOM).
- ✅ WoL desperta el PC objectiu (RTX 5090) de forma fiable, també repetidament
  (resolt el gating del bus suspès).
- ✅ El comandament es manté connectat durant l'arrencada → **un sol PS** (resolt
  l'apagat automàtic durant el boot).
- ✅ Amb el PC encès, tot funciona normal i el WoL s'avorta correctament.
- 🔬 **Release de rendiment (§3.4, 2026-06-24): pendent de validació a maquinari.**
  Build verificat al mapa (CELT-encode a RAM, SILK a flash, ~276 KB heap lliure) i
  flashejat. Cal passar la matriu de proves de maquinari (vegeu IMPROVEMENTS.md):
  àudio altaveu/mic/haptics, pair/reconnect, BOOTSEL, wake-on-PS, WoL en fred, DSE,
  config-save, i ≥10 cicles pair+WoL sense PANIC.

### Maquinari / muntatge validat
- Pico 2W amb el firmware de producció (`build/ds5-bridge.uf2`, `ENABLE_WOL=ON`).
- Alimentació estable recomanada: hub/extensor USB amb alimentació externa, o un
  port USB del PC amb "always-on USB" (ErP off) per donar corrent standby.

---

## 6. Registre de canvis (work log)

### 2026-06-23 — Port del WoL i estabilització a maquinari real
- Clonat el tag `v0.7.2-hotfix`; portat el WoL des de la feina sobre la 0.6.0.
- Compilat amb Pico SDK 2.2.0 + TinyUSB **0.20.0** + ARM GCC 14.2.Rel1 (vegeu BUILD.md).
- **Fix 3.1:** OOM resolt traient Opus de RAM quan `ENABLE_WOL`.
- **Fix 3.2:** gating del WoL ara `tud_mounted() && !tud_suspended()`; debounce 90 s.
- **Fix 3.3 (definitiu):** `wake_suppress_poweroff()` evita que el comandament
  s'apagui durant l'arrencada del PC.
- Afegides eines de diagnòstic `WOL_FORCE_TEST` i `WOL_UDP_LOG`.
- Validat a maquinari real: experiència d'un sol PS aconseguida.

### 2026-06-24 — Release de rendiment (àudio perfecte + headroom de RAM)
- Anàlisi exhaustiva multi-agent (7 dimensions, verificació adversarial) → pla per fases.
- **Relocació selectiva CELT-encode** d'Opus a RAM (§3.4): resol el compromís d'àudio
  del fix 3.1. `.data` 64 KB → 150 KB; heap lliure ~362 KB → **~276 KB**; àudio perfecte.
- Pont HCI btstack↔cyw43 a RAM; `ENABLE_LOG_INFO` tret; `<iostream>`/`print_hex` tret;
  barrera anti-LTO a CMake.
- Build net verificat (mapa: CELT a RAM, SILK a flash) i flashejat. Validació de
  maquinari pendent. Properes millores (mesura de stack, lwIP, decode/rodata) a
  IMPROVEMENTS.md.

> Antecedents (anàlisi original i revisió multi-agent) a la carpeta de la 0.6.0.

---

## 7. ⚠️ Seguretat / git

- `src/secrets.h` (SSID, **contrasenya WiFi**, MAC) està **gitignored** i **NO s'ha
  de pujar mai**. Plantilla pública: `src/secrets.h.example`.
- El remote `origin` apunta a l'upstream `awalol/DS5Dongle`. Per pujar els canvis,
  **crea un repositori propi** i canvia el remote; no facis push a l'upstream.
