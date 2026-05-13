@echo off
call "C:\Program Files\Raspberry Pi\Pico SDK v1.5.1\pico-env.cmd"
set "PICO_SDK_PATH=C:\Users\zurce\.pico-sdk\sdk"
set "PATH=%PATH%;C:\Users\zurce\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.MCF.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
cd /d C:\Users\zurce\Code\micropython\DS5Dongle
if exist build rmdir /s /q build
mkdir build
cd build
cmake -G "Ninja" ..
ninja
