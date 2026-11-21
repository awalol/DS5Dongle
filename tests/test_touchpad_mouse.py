"""Host regression: python tests/test_touchpad_mouse.py (requires g++ on PATH)."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('config_tool', ROOT / 'tools/config_tool.py')
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

PLATFORM = '''#pragma once
#include <cstdint>
#include <cstring>
#include "touchpad_mouse.h"
using absolute_time_t = uint64_t;
constexpr absolute_time_t nil_time = 0;
inline uint64_t now_us = 0;
inline absolute_time_t get_absolute_time() { return now_us; }
inline uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t) (t / 1000); }
inline absolute_time_t make_timeout_time_ms(uint32_t ms) { return now_us + ms * 1000ull; }
inline bool time_reached(absolute_time_t t) { return now_us >= t; }
struct TouchFingerData {
    uint32_t Index : 7;
    uint32_t NotTouching : 1;
    uint32_t FingerX : 12;
    uint32_t FingerY : 12;
};
struct TouchData { TouchFingerData Finger[2]; uint8_t Timestamp; };
struct USBGetStateData {
    uint8_t ButtonHome : 1;
    uint8_t ButtonPad : 1;
    TouchData Touch;
};
struct Config_body { uint8_t touchpad_mouse = 0; uint8_t touchpad_mouse_speed = 10; };
inline Config_body cfg;
inline Config_body& get_config() { return cfg; }
inline bool usb_keyboard_only = false, usb_reconfiguring = false;
inline bool ready = true, mounted = true, suspended = false;
inline bool tud_mounted() { return mounted; }
inline bool tud_suspended() { return suspended; }
inline bool tud_hid_n_ready(uint8_t) { return ready; }
struct MouseOut { uint8_t buttons; int8_t x, y, wheel, pan; };
inline MouseOut out{};
inline long total_x = 0, total_y = 0, total_wheel = 0, total_pan = 0;
inline unsigned nreports = 0;
inline bool tud_hid_n_report(uint8_t instance, uint8_t id, const void* data, uint16_t len) {
    if (instance != 2 || id != TOUCHPAD_MOUSE_REPORT_ID || len != sizeof(MouseOut)) return false;
    memcpy(&out, data, sizeof(out));
    total_x += out.x; total_y += out.y; total_wheel += out.wheel; total_pan += out.pan;
    ++nreports;
    return true;
}
'''


class TouchpadMouseTests(unittest.TestCase):
    def test_config_fields(self):
        names = tool.FIELD_NAMES
        self.assertEqual(names[-2:], ['touchpad_mouse', 'touchpad_mouse_speed'])
        cfg = {name: 0 for name in names}
        cfg.update(haptics_gain=1.0, audio_buffer_length=48, touchpad_mouse=2,
                   touchpad_mouse_speed=10)
        self.assertEqual(tool.unpack_config(tool.pack_config(cfg)), cfg)
        cfg['touchpad_mouse'] = 3
        with self.assertRaises(ValueError):
            tool.pack_config(cfg)

    def test_firmware(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            # Compile the real gesture logic, substituting only platform headers.
            for name in ('touchpad_mouse.cpp', 'touchpad_mouse.h'):
                shutil.copyfile(ROOT / 'src' / name, tmp / name)
            (tmp / 'pico').mkdir()
            for name in ('config.h', 'tusb.h', 'usb.h', 'utils.h', 'pico/time.h'):
                (tmp / name).write_text('#include "platform.h"\n')
            (tmp / 'platform.h').write_text(PLATFORM)
            shutil.copyfile(ROOT / 'tests/touchpad_mouse_test.cpp', tmp / 'test.cpp')
            exe = tmp / 'test.exe'
            subprocess.run(['g++', '-std=c++17', '-Wall', '-DENABLE_WAKE_HID', '-I', str(tmp),
                            str(tmp / 'touchpad_mouse.cpp'), str(tmp / 'test.cpp'),
                            '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
            # Without the wake HID interfaces only the enumeration flag remains.
            subprocess.run(['g++', '-std=c++17', '-I', str(tmp), '-c',
                            str(tmp / 'touchpad_mouse.cpp'), '-o', str(tmp / 'standard.o')],
                           check=True)


if __name__ == '__main__':
    unittest.main()
