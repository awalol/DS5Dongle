# Compilació i flasheig — DS5Dongle v0.7.2-hotfix (+ Wake-on-LAN)

Per al funcionament i els fixos, vegeu [CONTEXT.md](CONTEXT.md). Per a millores
pendents, [IMPROVEMENTS.md](IMPROVEMENTS.md).

> **Entorn validat (Windows 11):** extensió **Raspberry Pi Pico** de VS Code, que
> instal·la tot a `~/.pico-sdk`. SDK **2.2.0**, TinyUSB **0.20.0**, ARM GCC
> **14.2.Rel1**, CMake 3.31.5, Ninja 1.12.1.

---

## 0. TL;DR (PowerShell, Windows)

```powershell
# 1) Eines al PATH d'aquesta sessio (l'extensio de VS Code les instal.la aqui)
$env:PATH = "$env:USERPROFILE\.pico-sdk\cmake\v3.31.5\bin;" +
            "$env:USERPROFILE\.pico-sdk\ninja\v1.12.1;" +
            "$env:USERPROFILE\.pico-sdk\toolchain\14_2_Rel1\bin;" + $env:PATH

# 2) Credencials (nomes el primer cop)
Copy-Item src\secrets.h.example src\secrets.h     # i edita src\secrets.h

# 3) Configurar + compilar (produccio, WoL ON)
cd "C:\path\to\DS5Dongle-0.7.2-hotfix"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_WOL=ON -DPICO_BOARD=pico2_w
ninja -C build ds5-bridge
# -> binari: build\ds5-bridge.uf2
```

> Les versions de carpeta (`v3.31.5`, `v1.12.1`, `14_2_Rel1`) poden variar; comprova
> els noms reals dins `~/.pico-sdk\{cmake,ninja,toolchain}` i ajusta el PATH.

---

## 1. Prerequisits

| Eina | Versió |
|------|--------|
| Raspberry Pi **Pico SDK** | **2.2.0** |
| **TinyUSB** (dins el SDK) | **0.20.0** (obligatori) |
| **ARM GNU Toolchain** | `arm-none-eabi` 14.2.Rel1 |
| **CMake** ≥ 3.13, **Ninja**, **Python 3** | — |
| Submòduls | `lib/WDL`, `lib/opus` |

### Fixar TinyUSB a 0.20.0 (si el SDK en porta una altra)
El SDK 2.2.0 sol portar TinyUSB 0.18.0; **cal 0.20.0** o falla USB/àudio:
```powershell
Set-Location "$env:USERPROFILE\.pico-sdk\sdk\2.2.0\lib\tinyusb"
git fetch --depth 1 origin refs/tags/0.20.0:refs/tags/0.20.0
git checkout --detach tags/0.20.0
```

### Submòduls del projecte
```powershell
git submodule update --init --recursive    # poblar lib/WDL i lib/opus
```

---

## 2. (WoL) Omplir `src/secrets.h`

```powershell
Copy-Item src\secrets.h.example src\secrets.h
```
Edita `src\secrets.h` amb el teu SSID, contrasenya i la **MAC** del PC a despertar
(`ipconfig /all` → "Adreça física" de l'adaptador amb WoL armat, normalment
l'Ethernet). El WoL s'adreça a la MAC, no a la IP. Fitxer **gitignored**.

---

## 3. Compilar

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_WOL=ON -DPICO_BOARD=pico2_w
ninja -C build ds5-bridge
```
Resultat: **`build\ds5-bridge.uf2`**.

### Opcions (`-D...`)

| Opció | Defecte | Efecte |
|-------|---------|--------|
| `ENABLE_WOL` | **ON** | Wake-on-LAN; lwIP; **Opus CELT-encode rellocat a RAM** (selectiu, ~87 KB → àudio perfecte, ~276 KB heap lliure). Vegeu CONTEXT.md §3.4 |
| `ENABLE_SERIAL` | OFF | Consola USB CDC per a `printf` (canvia l'USB; desactiva el watchdog) |
| `ENABLE_VERBOSE` | OFF | Logs BTstack detallats |
| `ENABLE_BATT_LED` | ON | LED de bateria baixa |
| `WOL_FORCE_TEST` | OFF | **DEBUG:** força el WoL sempre (ignora el gating del PC encès) |
| `WOL_UDP_LOG` | OFF | **DEBUG:** `printf` → UDP broadcast:9999 + manté WiFi amunt |
| `PICO_BOARD` | — | `pico2_w` per a la Pico 2W |
| `WAKE_DEBUG` | OFF | Traça de la FSM de wake |

> ⚠️ **Build net en canviar `ENABLE_WOL`:** la rellocació d'Opus modifica
> `libopus.a` *in place*. Si passes de WoL OFF↔ON, esborra primer la carpeta:
> `Remove-Item -Recurse -Force build` i torna a configurar.

---

## 4. Flashejar

La Pico 2W en **mode BOOTSEL** es munta com a unitat (etiqueta `RP2350`).

1. Desendolla la Pico → mantén premut **BOOTSEL** → endolla-la → apareix la unitat
   (p. ex. `E:`).
2. Copia-hi **`build\ds5-bridge.uf2`**:
   ```powershell
   Copy-Item build\ds5-bridge.uf2 E:\
   ```
3. Es reinicia sola amb el nou firmware.

> El dispositiu USB de comandament **només apareix quan el comandament està
> connectat** (per disseny). Amb `ENABLE_SERIAL=ON` apareix també un port COM.

---

## 5. Ús normal (experiència d'un sol PS)

1. PC **apagat**, Pico alimentat (port "always-on USB" o hub/extensor alimentat).
2. Prem **PS** al comandament → connecta amb la Pico → s'envia el WoL → el PC
   s'encén → el comandament es manté connectat durant l'arrencada.
3. Amb el PC encès, el WiFi es tomba i tot funciona com un dongle normal.

---

## 6. Diagnòstic (sense adaptador UART)

Com que el WoL passa amb el **PC objectiu apagat**, els logs no surten per USB. Dues
vies afegides aquest projecte:

### 6.1 Logs per WiFi (UDP) — `WOL_UDP_LOG`
Build de diagnòstic que envia `printf` per UDP broadcast i manté el WiFi amunt:
```powershell
cmake -S . -B build-udplog -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_WOL=ON -DWOL_UDP_LOG=ON -DPICO_BOARD=pico2_w
ninja -C build-udplog ds5-bridge
```
Al PC de captura (mateixa LAN que la WiFi del Pico):
```powershell
# Un sol cop, com a ADMINISTRADOR: obrir el port al tallafocs
New-NetFirewallRule -DisplayName "WoL-UDP-Log-9999" -Direction Inbound -Protocol UDP -LocalPort 9999 -Action Allow

# Escoltador (no cal admin); escriu a wol-boot.log
$udp = New-Object System.Net.Sockets.UdpClient
$udp.Client.Bind((New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 9999)))
$ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
while ($true) { $d = $udp.Receive([ref]$ep); [System.Text.Encoding]::ASCII.GetString($d) }
```
> ⚠️ `WOL_UDP_LOG` manté el WiFi actiu → estressa la coexistència BT i pot provocar
> desconnexions que en producció NO hi són. És només per veure motius de
> desconnexió, no per mesurar estabilitat.

### 6.2 Forçar el WoL amb el PC encès — `WOL_FORCE_TEST`
Per validar el camí WiFi + magic packet sense apagar el PC de proves:
```powershell
cmake -S . -B build-test -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_WOL=ON -DWOL_FORCE_TEST=ON -DENABLE_SERIAL=ON -DPICO_BOARD=pico2_w
ninja -C build-test ds5-bridge
```

### 6.3 Logs per USB CDC — `ENABLE_SERIAL`
Amb el PC **encès**, `printf` surt per un port COM. Per llegir-lo (PowerShell):
```powershell
$p = New-Object System.IO.Ports.SerialPort 'COM6',115200,'None',8,'One'
$p.DtrEnable=$true; $p.Open()
while($true){ $p.ReadExisting() }
```

---

## 7. Resolució de problemes

| Símptoma | Causa | Solució |
|----------|-------|---------|
| `*** PANIC *** Out of memory` / es penja en emparellar | **Tot** Opus a RAM + lwIP → heap esgotat | Amb `ENABLE_WOL=ON` només es relloca el camí CELT-encode (~87 KB, ~276 KB heap lliure). Si reapareix, **build net** i revisa que no s'hagin afegit més TUs/`.rodata` a la relocació (CONTEXT.md §3.4, IMPROVEMENTS.md P3) |
| CMake: "unable to find a build program / Ninja" | Eines fora del PATH | Posa cmake/ninja/toolchain al PATH (§0) |
| `cd : no existe ...\.pico-sdk\...` | `$USERPROFILE` en comptes de `$env:USERPROFILE` | Usa `$env:USERPROFILE` a PowerShell |
| Errors USB/àudio | TinyUSB ≠ 0.20.0 | §1 |
| El WoL no es dispara el 2n cop | (resolt) gating del bus suspès | Cal `tud_mounted() && !tud_suspended()` (ja aplicat) |
| Cal prémer PS 2 cops en despertar | (resolt) apagat del comandament als 3 s de suspensió | `wake_suppress_poweroff()` (ja aplicat) |
| El WoL no desperta el PC | MAC dolenta / sense standby USB / subxarxa | Revisa MAC; corrent standby; mateixa subxarxa L2 |
| Semàfor en obrir COM | Firmware serial penjat o port ocupat | Replug del Pico; tanca altres processos del COM |
