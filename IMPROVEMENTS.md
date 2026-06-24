# Millores pendents / full de ruta — DS5Dongle WoL

Estat actual: **WoL funcional i validat** (un sol PS desperta el PC i el comandament
queda connectat). Vegeu [CONTEXT.md](CONTEXT.md). Aquí, idees per a properes sessions,
per prioritat.

---

## P1 — Recuperar qualitat d'àudio mantenint el WoL (compromís actual)

**Context:** amb `ENABLE_WOL=ON` Opus corre des de flash (no de RAM) per alliberar
els ~280 KB que necessitava lwIP. Possible petita pèrdua de qualitat a l'altaveu/jack
**del comandament** (la resta no s'afecta).

1. **Primer, mesurar si cal:** provar a l'ús real si l'àudio del comandament (altaveu
   o auriculars al jack) té talls. A 150 MHz amb la cache XIP pot ser perfecte. Si no
   es nota, **no cal fer res**.
2. **Si cal, rellocació selectiva d'Opus a RAM** (millor opció): en comptes de tot
   `libopus` (~280 KB), rellocar només les funcions calentes del camí de codificació
   per frame (celt_encoder, mdct, kiss_fft, pitch, vq, bands, quant_bands…) amb el
   mateix patró `relocate_to_ram()` que ja s'usa per a btstack/cyw43/tinyusb al
   [CMakeLists.txt](CMakeLists.txt). Amb WoL hi ha ~362 KB lliures: es poden tornar
   ~150–200 KB d'Opus a RAM i mantenir marge de sobres.
3. **Alternativa:** IP estàtica + lwIP mínim (vegeu P2) per recuperar prou RAM i
   tornar **tot** Opus a RAM (més just; cal validar que no torni l'OOM).

---

## P2 — lwIP més petit i WoL més robust

- **IP estàtica per al Pico** (en comptes de DHCP) a [lwipopts.h](src/lwipopts.h):
  elimina la maquinària i els buffers de recepció del DHCP, permet baixar
  `PBUF_POOL_SIZE` a ~2 i estalvia RAM. Per al broadcast WoL n'hi ha prou amb tenir
  el netif amunt. Caldria demanar una IP lliure de la xarxa.
- **Netejar la supressió de l'apagat:** ara `wake_suppress_poweroff(180s)` és un
  temporitzador fix. Millor: cancel·lar-la explícitament quan `tud_mount_cb` confirma
  que el host ha arrencat (ja arriba a `wake.cpp`), per no allargar-la innecessàriament.
- **WoL més ràpid:** avaluar reduir `HOST_OBSERVE_US` (3 s) si el gating
  `!tud_suspended()` ja és prou fiable per si sol.

---

## P3 — Validació / matriu de proves

- **Suspensió S3 (dormir, no apagar):** confirmar que el "wake per USB" (wake.cpp)
  segueix funcionant i que la supressió de l'apagat NO trenca el comportament normal
  (el comandament s'ha d'apagar als 3 s en una suspensió real sense WoL).
- **PC que no arrenca:** confirmar que si el WoL falla, el comandament s'apaga passats
  els 180 s (estalvi de bateria) i no es queda encès indefinidament.
- **DualSense normal vs Edge:** als logs apareix `[DSE] HID HANDSHAKE` també amb un
  DS5 normal; confirmar que el camí no-Edge és net i no causa reconnexions.

---

## P4 — Qualitat de vida / preparació per a upstream

- Els fixos 3.2 (gating `!tud_suspended()`) i 3.3 (`wake_suppress_poweroff`) són
  millores **generals**; es podrien proposar a l'upstream `awalol/DS5Dongle` via PR.
- Indicador visual: patró de LED del Pico distintiu per a "WoL enviat / esperant boot".
- `WOL_UDP_LOG` i `WOL_FORCE_TEST` són només DEBUG (OFF per defecte); deixar-ho clar
  o moure'ls a un bloc de diagnòstic ben marcat abans d'un PR.

---

## Notes per reprendre ràpid

- Firmware de producció: `cmake -S . -B build ... -DENABLE_WOL=ON` + `ninja -C build`.
- Recorda el **build net** si canvies `ENABLE_WOL` (Opus relocate és in-place a libopus.a).
- PATH de les eines i flasheig: [BUILD.md](BUILD.md) §0 i §4.
- `src/secrets.h` és local i gitignored; **mai** a git.
