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
- ⚠️ **Compromís:** amb WoL, el còdec d'àudio corre des de flash → possible petita
  pèrdua de qualitat a **l'altaveu/jack del propi comandament**. L'entrada del
  comandament, la vibració, els gallets i tota la resta **NO** s'afecten. Candidat
  de millora a [IMPROVEMENTS.md](IMPROVEMENTS.md) (rellocació selectiva).
- **Nota de build:** l'`objcopy` que renombrava les seccions modificava `libopus.a`
  *in place*; cal **build net** (esborrar `build*/`) perquè el canvi tingui efecte.

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
| `ENABLE_WOL` | **ON** | Wake-on-LAN; lwIP; **Opus a flash** (allibera RAM) |
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
- ⚠️ Pendent de valorar a l'ús: qualitat de l'àudio de l'altaveu/jack del comandament
  amb Opus des de flash (vegeu [IMPROVEMENTS.md](IMPROVEMENTS.md)).

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

> Antecedents (anàlisi original i revisió multi-agent) a la carpeta de la 0.6.0.

---

## 7. ⚠️ Seguretat / git

- `src/secrets.h` (SSID, **contrasenya WiFi**, MAC) està **gitignored** i **NO s'ha
  de pujar mai**. Plantilla pública: `src/secrets.h.example`.
- El remote `origin` apunta a l'upstream `awalol/DS5Dongle`. Per pujar els canvis,
  **crea un repositori propi** i canvia el remote; no facis push a l'upstream.
