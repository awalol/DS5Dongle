# Build and Flash — DS5Dongle v0.7.2-hotfix (+ Wake-on-LAN)

A Windows-focused build & diagnostics guide. For the Wake-on-LAN feature and its
`secrets.h` setup, see the README.

> **Tested environment (Windows 11):** the VS Code **Raspberry Pi Pico** extension, which installs everything under `~/.pico-sdk`. SDK **2.2.0**, TinyUSB **0.20.0**, ARM GCC **14.2.Rel1**, CMake 3.31.5, Ninja 1.12.1.

---

## 0. TL;DR (PowerShell, Windows)

```powershell
# 1) Put the tools on PATH for this session (the VS Code extension installs them here)
$env:PATH = "$env:USERPROFILE\.pico-sdk\cmake\v3.31.5\bin;" +
            "$env:USERPROFILE\.pico-sdk\ninja\v1.12.1;" +
            "$env:USERPROFILE\.pico-sdk\toolchain\14_2_Rel1\bin;" + $env:PATH

# 2) Credentials (first time only)
Copy-Item src\secrets.h.example src\secrets.h     # then edit src\secrets.h

# 3) Configure + build (production, WoL ON)
cd "C:\path\to\DS5Dongle-0.7.2-hotfix"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_WOL=ON -DPICO_BOARD=pico2_w
ninja -C build ds5-bridge
# -> binary: build\ds5-bridge.uf2
```

> The folder versions (`v3.31.5`, `v1.12.1`, `14_2_Rel1`) may differ. Check the actual names under `~/.pico-sdk\{cmake,ninja,toolchain}` and adjust the PATH accordingly.

---

## 1. Prerequisites

| Tool | Version |
|------|--------|
| Raspberry Pi **Pico SDK** | **2.2.0** |
| **TinyUSB** (bundled with the SDK) | **0.20.0** (required) |
| **ARM GNU Toolchain** | `arm-none-eabi` 14.2.Rel1 |
| **CMake** ≥ 3.13, **Ninja**, **Python 3** | — |
| Submodules | `lib/WDL`, `lib/opus` |

### Pin TinyUSB to 0.20.0 (if the SDK ships a different version)
SDK 2.2.0 usually ships TinyUSB 0.18.0; **0.20.0 is required** or USB/audio breaks:
```powershell
Set-Location "$env:USERPROFILE\.pico-sdk\sdk\2.2.0\lib\tinyusb"
git fetch --depth 1 origin refs/tags/0.20.0:refs/tags/0.20.0
git checkout --detach tags/0.20.0
```

### Project submodules
```powershell
git submodule update --init --recursive    # populate lib/WDL and lib/opus
```

---

## 2. (WoL) Fill in `src/secrets.h`

```powershell
Copy-Item src\secrets.h.example src\secrets.h
```
Edit `src\secrets.h` with your SSID, password and the **MAC** of the PC you want to wake (`ipconfig /all` → "Physical Address" of the adapter that has WoL armed, usually the Ethernet one). WoL targets the MAC, not the IP. This file is **gitignored**.

---

## 3. Build

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_WOL=ON -DPICO_BOARD=pico2_w
ninja -C build ds5-bridge
```
Output: **`build\ds5-bridge.uf2`**.

### Options (`-D...`)

| Option | Default | Effect |
|-------|---------|--------|
| `ENABLE_WOL` | **OFF** | Wake-on-LAN (opt-in: `-DENABLE_WOL=ON`); lwIP; **Opus CELT encode+decode relocated to RAM** (selective, ~104 KB → clean audio, ~260 KB heap free) |
| `WOL_ALWAYS` | OFF | Skip the PC-powered-on gate and always send the magic packet on a (debounced) controller connect. **Use this if WoL never fires on your machine:** boards that keep USB powered *and active* while off (S5 "power on by USB keyboard" ports, Modern Standby) look "on" to the gate forever. Harmless when the PC is already on. |
| `ENABLE_SERIAL` | OFF | USB CDC console for `printf` (changes the USB descriptor; disables the watchdog) |
| `ENABLE_VERBOSE` | OFF | Detailed BTstack logs |
| `ENABLE_BATT_LED` | ON | Low-battery LED |
| `WOL_FORCE_TEST` | OFF | **DEBUG:** always force WoL (ignores the PC-on gating; use `WOL_ALWAYS` for production) |
| `WOL_UDP_LOG` | OFF | **DEBUG:** `printf` → UDP broadcast:9999 and keeps WiFi up |
| `PICO_BOARD` | — | `pico2_w` for the Pico 2W |
| `WAKE_DEBUG` | OFF | Wake FSM trace |

> ⚠️ **Do a clean build when changing `ENABLE_WOL`:** the Opus relocation patches `libopus.a` *in place*. When switching WoL OFF↔ON, delete the build directory first: `Remove-Item -Recurse -Force build`, then reconfigure. (The build now also *enforces* this: reconfiguring an existing build dir across strategies fails with a clear error, and a post-link check verifies the hot-path symbols really landed in RAM.)

---

## 4. Flash

In **BOOTSEL mode** the Pico 2W mounts as a drive (labelled `RP2350`).

1. Unplug the Pico → hold **BOOTSEL** → plug it back in → the drive appears (e.g. `E:`).
2. Copy **`build\ds5-bridge.uf2`** onto it:
   ```powershell
   Copy-Item build\ds5-bridge.uf2 E:\
   ```
3. It reboots itself into the new firmware.

> The controller USB device **only shows up when the controller is connected** (by design). With `ENABLE_SERIAL=ON` a COM port appears as well.

---

## 5. Normal use (single-PC experience)

1. PC **off**, Pico powered (an always-on USB port or a powered hub/extender).
2. Press **PS** on the controller → it connects to the Pico → the WoL packet is sent → the PC powers on → the controller stays connected through boot.
3. Once the PC is on, WiFi shuts down and everything works like an ordinary dongle.

---

## 6. Diagnostics (no UART adapter)

Because WoL happens with the **target PC off**, logs can't come out over USB. This project adds two routes:

### 6.1 Logs over WiFi (UDP) — `WOL_UDP_LOG`
A diagnostic build that sends `printf` over UDP broadcast and keeps WiFi up:
```powershell
cmake -S . -B build-udplog -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_WOL=ON -DWOL_UDP_LOG=ON -DPICO_BOARD=pico2_w
ninja -C build-udplog ds5-bridge
```
On the capture PC (same LAN as the Pico's WiFi):
```powershell
# Once, as ADMINISTRATOR: open the firewall port
New-NetFirewallRule -DisplayName "WoL-UDP-Log-9999" -Direction Inbound -Protocol UDP -LocalPort 9999 -Action Allow

# Listener (no admin needed); writes to wol-boot.log
$udp = New-Object System.Net.Sockets.UdpClient
$udp.Client.Bind((New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 9999)))
$ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
while ($true) { $d = $udp.Receive([ref]$ep); [System.Text.Encoding]::ASCII.GetString($d) }
```
> ⚠️ `WOL_UDP_LOG` keeps WiFi active, which stresses BT coexistence and can cause disconnections that do NOT happen in production. Use it only to see why a disconnection occurred, not to measure stability.

### 6.2 Force WoL with the PC on — `WOL_FORCE_TEST`
To validate the WiFi + magic-packet path without powering off the test PC:
```powershell
cmake -S . -B build-test -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_WOL=ON -DWOL_FORCE_TEST=ON -DENABLE_SERIAL=ON -DPICO_BOARD=pico2_w
ninja -C build-test ds5-bridge
```

### 6.3 Logs over USB CDC — `ENABLE_SERIAL`
With the PC **on**, `printf` comes out on a COM port. To read it (PowerShell):
```powershell
$p = New-Object System.IO.Ports.SerialPort 'COM6',115200,'None',8,'One'
$p.DtrEnable=$true; $p.Open()
while($true){ $p.ReadExisting() }
```

---

## 7. Troubleshooting

| Symptom | Cause | Fix |
|----------|-------|---------|
| `*** PANIC *** Out of memory` / hangs while pairing | **All** of Opus in RAM + lwIP → heap exhausted | With `ENABLE_WOL=ON` only the CELT encode+decode path is relocated (~104 KB, ~260 KB heap free). If it returns, do a **clean build** and check that no extra TUs/`.rodata` have been added to the relocation member list in `CMakeLists.txt` |
| CMake: "unable to find a build program / Ninja" | Tools not on PATH | Put cmake/ninja/toolchain on PATH (§0) |
| `cd : cannot find ...\.pico-sdk\...` | `$USERPROFILE` instead of `$env:USERPROFILE` | Use `$env:USERPROFILE` in PowerShell |
| USB/audio errors | TinyUSB ≠ 0.20.0 | §1 |
| WoL doesn't fire the second time | (resolved) gating on the suspended bus | Requires `tud_mounted() && !tud_suspended()` (already applied) |
| Have to press PS twice to wake | (resolved) controller powered off 3 s into suspend | `wake_suppress_poweroff()` (already applied) |
| **WoL never fires at all** (serial/UDP log shows `USB host active (PC on): WoL aborted` with the PC off) | Your board keeps the USB bus powered **and active** in S5 ("power on by USB keyboard" ports, Modern Standby) — the PC-on gate cannot distinguish that from a running PC | Rebuild with `-DWOL_ALWAYS=ON` (supported mode; a magic packet to a running PC is harmless) |
| Silent WoL no-op after flashing | placeholder credentials (`secrets.h` missing or unedited) | The boot log prints `[WoL] secrets.h not configured ... WoL disabled`; fill in `src/secrets.h` and rebuild |
| WoL doesn't wake the PC | bad MAC / no USB standby power / wrong subnet | Check the MAC; standby current; same L2 subnet |
| Stall when opening COM | serial firmware hung or port busy | Replug the Pico; close other processes using the COM port |