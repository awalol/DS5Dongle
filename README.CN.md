# Pico2W DualSense 5 Bridge
[English](./README.md)
> 将 Pico2W 变成 DS5 手柄的无线适配器

# 功能特点
 - 支持HD震动

# 使用方法
1. 按住 Pico 上的BOOTSEL进入刷机
2. 将 .uf2 文件拖入进去
3. 将 DS5 手柄进入蓝牙配对模式
4. Enjoy it

***你可能需要在控制器处于匹配模式时重新插拔 pico***

- 手柄连接到pico以后，系统才会显示设备

# Pico 配置调整
你可以通过网页调整Pico的内部设置

- 用于正式固件: https://ds5.awalol.eu.org
- 用于测试固件: https://ds5-dev.awalol.eu.org

**忽略音量调节**（`lock_volume`）支持三种模式：

- `0`：关闭。
- `1`：游戏/软件——忽略 HID 输出报文中的音量、静音修改，仍可通过系统 USB Audio 调节。
- `2`：UAC（系统音量）——忽略 USB Audio 的音量、静音修改（包括 Windows 设备音量），
  游戏/软件的 HID 控制仍然生效。

两种忽略模式均覆盖扬声器、耳机和麦克风。请先设置好设备音量和静音状态，再选择忽略 UAC 调节。
USB Audio 查询会返回最后一次接受的 USB Audio 值。Windows 滑块可能仍会移动，因为主机
可能缓存自己请求的值。忽略 UAC 调节无法阻止游戏或 Windows 应用音量混合器在音频到达适配器之前
对音频采样进行衰减。

### Pico W 版本

Pico W 由于性能问题，只能支持震动，不支持扬声器。
你可以通过开启 `-DPICO_W_BUILD=ON` 编译项去开启 Pico W 固件编译，或者在 Github Action 下载预编译的固件

### USB 唤醒支持
这是一项实验性的功能。如果你需要该功能，请前往 feat/usb-wake 分支进行编译，或者使用该分支对应的 Github Action 预编译固件。`ds5-bridge-wake.uf2` 为该功能的固件

极为建议在使用该功能前阅读  #60 和 #61

### 社区分支
https://github.com/MarcelineVPQ/DS5Dongle-OLED-Edition
https://github.com/zurce/DS5Dongle-OLED

# 当前问题:
- 声音可能有点小卡顿
- 由于编码需要，需要对pico进行超频，当前的参数是1.2V 320MHz。
- 若您的pico使用该超频参数无法启动，请自行增加电压或者降低频率

# 未来计划
请查看[DS5Dongle plan](https://github.com/users/awalol/projects/5)

# 编译

需要 Pico SDK **2.3.1**，并把 SDK 里的 TinyUSB 子模块切换到 `98741b55`（SDK 自带的子模块仍指向 0.18.0，版本太旧）：

```sh
git -C <sdk>/lib/tinyusb fetch --depth 1 origin 98741b55959564746998d0453c32d87064629d7c
git -C <sdk>/lib/tinyusb checkout --detach 98741b55959564746998d0453c32d87064629d7c
```

然后初始化本仓库的子模块并编译：

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_SDK_PATH=<sdk>
cmake --build build --target ds5-bridge
```

Windows 下可直接运行 `tools\build-windows.ps1`，macOS 下运行 `tools/build-macos.sh`——两个脚本都会自动准备上述版本的 SDK 与 TinyUSB。

# 致谢
 - [rafaelvaloto/Pico_W-Dualsense](https://github.com/rafaelvaloto/Pico_W-Dualsense) - 灵感来源
 - [egormanga/SAxense](https://github.com/egormanga/SAxense) - 震动报文
 - [https://controllers.fandom.com/wiki/Sony_DualSense](https://controllers.fandom.com/wiki/Sony_DualSense) - 数据报文结构
 - [Paliverse/DualSenseX](https://github.com/Paliverse/DualSenseX) - 扬声器数据包报文
