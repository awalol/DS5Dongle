# Pico2W DualSense 5 Bridge
> 将 Pico2W 变成 DS5 手柄的无线适配器

# 功能特点
 - 支持HD震动

# 使用方法
1. 按住 Pico 上的BOOTSEL进入刷机
2. 将 .uf2 文件拖入进去
3. 将 DS5 手柄进入蓝牙配对模式
4. Enjoy it

注意：在手柄与pico连接以后，电脑才会识别到设备

- 调整扬声器音量为改变震动增益倍数，范围 [1,2]
- 开启扬声器静音为关闭LED连接提示 (手柄重连后生效)
- 开启麦克风静音为禁用静默断连

# 当前问题:
当前无，待反馈

# 未来计划

# 编译
需要将pico sdk里面的tinyusb版本升级到最新

# 致谢
 - [rafaelvaloto/Pico_W-Dualsense](https://github.com/rafaelvaloto/Pico_W-Dualsense) - 灵感来源
 - [egormanga/SAxense](https://github.com/egormanga/SAxense) - 震动报文
 - [https://controllers.fandom.com/wiki/Sony_DualSense](https://controllers.fandom.com/wiki/Sony_DualSense) - 数据报文结构