import struct
import hid          #python-hidapi is needed, not python-hid
import sys

# Sony Vendor ID
SONY_VID = 0x054C
# Product IDs
DS5_PID = 0x0CE6      # DualSense
DSE_PID = 0x0DF2      # DualSense Edge


def open_device():
    """打开 DS5 或 DSE 设备"""
    device = hid.device()
    
    # 尝试打开 DS5
    try:
        device.open(SONY_VID, DS5_PID)
        print("[INFO] Connected to DualSense (DS5)")
        return device, "DS5"
    except:
        pass
    
    # 尝试打开 DSE
    try:
        device.open(SONY_VID, DSE_PID)
        print("[INFO] Connected to DualSense Edge (DSE)")
        return device, "DSE"
    except:
        pass
    
    print("[ERROR] No DS5/DSE device found!")
    sys.exit(1)


def get_config(device):
    """读取配置"""
    raw_data = device.get_feature_report(0xF7, 64)
    # hidapi 返回的是 list，需要转换为 bytes
    raw_bytes = bytes(raw_data)
    # 跳过 Report ID (第一个字节)
    config_bytes = raw_bytes[1:15]  # 14 字节的 Config_body
    
    # 解析配置
    haptics_gain = struct.unpack('<f', config_bytes[0:4])[0]
    speaker_volume_gain = struct.unpack('<f', config_bytes[4:8])[0]
    inactive_time = config_bytes[8]
    disable_inactive_disconnect = config_bytes[9]
    disable_pico_led = config_bytes[10]
    polling_rate_mode = config_bytes[11]
    haptics_buffer_length = config_bytes[12]
    controller_mode = config_bytes[13]
    
    config = {
        'haptics_gain': haptics_gain,
        'speaker_volume_gain': speaker_volume_gain,
        'inactive_time': inactive_time,
        'disable_inactive_disconnect': disable_inactive_disconnect,
        'disable_pico_led': disable_pico_led,
        'polling_rate_mode': polling_rate_mode,
        'haptics_buffer_length': haptics_buffer_length,
        'controller_mode': controller_mode
    }
    
    return config


def print_config(config):
    """打印配置信息"""
    polling_modes = {0: "250Hz", 1: "500Hz", 2: "Real-time (1000Hz)"}
    controller_modes = {0: "DS5", 1: "DSE", 2: "Auto"}
    
    print("\n=== Current Configuration ===")
    print(f"haptics_gain:                {config['haptics_gain']:.2f} [1.0-2.0]")
    print(f"speaker_volume_gain:         {config['speaker_volume_gain']:.1f} dB [-100-0]")
    print(f"inactive_time:               {config['inactive_time']} min [5-60]")
    print(f"disable_inactive_disconnect: {config['disable_inactive_disconnect']} (0=enabled auto disconnect, 1=disabled auto disconnect)")
    print(f"disable_pico_led:            {config['disable_pico_led']} (0=LED on, 1=LED off)")
    print(f"polling_rate_mode:           {config['polling_rate_mode']} ({polling_modes.get(config['polling_rate_mode'], 'Unknown')})")
    print(f"haptics_buffer_length:       {config['haptics_buffer_length']} [16-128]")
    print(f"controller_mode:             {config['controller_mode']} ({controller_modes.get(config['controller_mode'], 'Unknown')})")
    print("=============================\n")


def set_config(device, **kwargs):
    """设置配置参数"""
    # 先读取当前配置
    current_config = get_config(device)
    
    # 更新指定的参数
    for key, value in kwargs.items():
        if key in current_config:
            current_config[key] = value
            print(f"[INFO] Setting {key} = {value}")
        else:
            print(f"[WARNING] Unknown config parameter: {key}")
    
    # 打包配置数据
    new_config = struct.pack('<ffBBBBBB',
        current_config['haptics_gain'],
        current_config['speaker_volume_gain'],
        current_config['inactive_time'],
        current_config['disable_inactive_disconnect'],
        current_config['disable_pico_led'],
        current_config['polling_rate_mode'],
        current_config['haptics_buffer_length'],
        current_config['controller_mode']
    )
    
    # 发送更新命令到内存
    print("[INFO] Updating config in memory...")
    device.send_feature_report(bytes([0xF6, 0x01]) + new_config)
    
    # 保存到 Flash
    print("[INFO] Saving config to flash...")
    device.send_feature_report(bytes([0xF6, 0x02]))
    
    # 重新连接 USB
    print("[INFO] Reconnecting USB device...")
    device.send_feature_report(bytes([0xF6, 0x03]))
    
    print("[SUCCESS] Configuration updated successfully!")


def main():
    """主函数"""
    # 打开设备
    device, device_type = open_device()
    
    try:
        # 默认执行 get 操作
        print(f"\n[INFO] Reading configuration from {device_type}...\n")
        config = get_config(device)
        print_config(config)
        
        # 如果需要设置配置，可以取消下面的注释并修改参数
        # print("\n[INFO] Updating configuration...")
        # set_config(
        #     device,
        #     haptics_gain=1.5,
        #     speaker_volume_gain=0.0,
        #     inactive_time=15,
        #     disable_inactive_disconnect=0,
        #     disable_pico_led=0,
        #     polling_rate_mode=2,
        #     haptics_buffer_length=64,
        #     controller_mode=2
        # )
        
    finally:
        device.close()


if __name__ == "__main__":
    main()