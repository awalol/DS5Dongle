"""Host regression: python tests/test_hold.py (requires g++ on PATH)."""
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


class HoldTests(unittest.TestCase):
    def test_protocol(self):
        for text in ('1=Cross@hold:Space', '2=L1@hold:Ctrl+C',
                     '3=L1+R1@hold:Shift', '4=Home:F13', '5=Home*2:F14'):
            slot, value = tool.parse_shortcut_assignment(text)
            self.assertTrue(tool.shortcut_slot_valid(value))
            data = bytearray(tool.SHORTCUT_STORAGE_SIZE)
            tool.pack_shortcut(data, slot, value)
            self.assertEqual(tool.unpack_shortcut(data, slot), value)
        for text in ('1=Cross*2@hold:Space', '1=L1@hold:VolumeUp',
                     '1=L1@hold:bt_disconnect'):
            with self.assertRaises(SystemExit):
                tool.parse_shortcut_assignment(text)

    def test_firmware(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            # Compile the real state machine, substituting only platform headers.
            for name in ('button_shortcut.cpp', 'button_shortcut.h', 'usb_descriptors.h'):
                shutil.copyfile(ROOT / 'src' / name, tmp / name)
            (tmp / 'pico').mkdir()
            for name in ('bt.h', 'button_utils.h', 'config.h', 'utils.h', 'usb.h',
                         'tusb.h', 'wake.h', 'pico/time.h'):
                (tmp / name).write_text('#include "platform.h"\n')
            (tmp / 'platform.h').write_text('''#pragma once
#include <cstdint>
#include <cstring>
#include "button_shortcut.h"
#include "usb_descriptors.h"
using absolute_time_t = uint64_t;
constexpr absolute_time_t nil_time = 0;
inline uint64_t now = 0;
inline auto make_timeout_time_ms(uint32_t ms) { return now + ms; }
inline bool time_reached(absolute_time_t t) { return now >= t; }
constexpr uint8_t Disable = 27, DPadNorthWest = 7;
struct USBGetStateData { uint32_t buttons = 0; };
inline bool button_is_pressed(const USBGetStateData& s, uint8_t b) {
    return b < 27 && (s.buttons & (1u << b));
}
struct Button { ButtonShortcut shortcuts[BUTTON_SHORTCUT_COUNT]{}; };
inline Button config;
inline Button& get_button() { return config; }
inline bool bt_disconnect() { return true; }
inline bool usb_keyboard_only = false, usb_reconfiguring = false;
inline bool ready = true, accept = true, mounted = true, suspended = false;
inline bool tud_mounted() { return mounted; }
inline bool tud_suspended() { return suspended; }
inline bool consumer_ready = true, wake_busy = false, auto_busy = false;
inline bool wake_keyboard_busy() { return wake_busy; }
inline bool tud_hid_n_ready(uint8_t instance) { return ready && (instance != 2 || consumer_ready); }
inline uint8_t mods = 0, keys[6]{};
inline unsigned reports = 0;
inline bool tud_hid_n_keyboard_report(uint8_t, uint8_t, uint8_t m, const uint8_t* k) {
    if (!accept) return false;
    mods = m; memcpy(keys, k, 6); ++reports;
    if (auto_busy) ready = false;
    return true;
}
inline uint16_t consumer_usage = 0;
inline bool tud_hid_n_report(uint8_t, uint8_t, const void* data, uint16_t) {
    if (!accept) return false;
    memcpy(&consumer_usage, data, sizeof(consumer_usage)); return true;
}
''')
            shutil.copyfile(ROOT / 'tests/hold_state_test.cpp', tmp / 'test.cpp')
            exe = tmp / 'test.exe'
            subprocess.run(['g++', '-std=c++17', '-DENABLE_WAKE_HID', '-I', str(tmp),
                            str(tmp / 'button_shortcut.cpp'), str(tmp / 'test.cpp'),
                            '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
            # The standard build must keep validating config without HID references.
            subprocess.run(['g++', '-std=c++17', '-I', str(tmp), '-c',
                            str(tmp / 'button_shortcut.cpp'), '-o', str(tmp / 'standard.o')],
                           check=True)


if __name__ == '__main__':
    unittest.main()
