#!/usr/bin/env python3
"""
Read and modify the ds5dongle configuration over USB HID, without reflashing.

Protocol (see src/cmd.cpp / src/config.h):
  GET feature report 0xF7 -> raw Config_body bytes
  GET feature report 0xF8 -> firmware version string
  GET/SET feature report 0xFA -> button remap table (28 bytes)
  GET/SET feature report 0xFB -> shortcut slots (63 bytes)
  SET feature report 0xF6:
      funcid 0x01 + body   -> update config in RAM (firmware clamps invalid values)
      funcid 0x02          -> persist config to flash
      funcid 0x03          -> reconnect the USB device

Config_body is a packed struct; this tool derives the binary layout from FIELDS.

Requires: pip install hidapi

Examples:
  python config_tool.py get
  python config_tool.py set speaker_volume=90 enable_wake=1
  python config_tool.py set haptics_gain=1.5 --no-save
  python config_tool.py remap
  python config_tool.py remap square=cross l1=disable
  python config_tool.py remap square=nomap  # clear (restore identity mapping)
  python config_tool.py remap --reset       # reset all button remaps
  python config_tool.py shortcut
  python config_tool.py shortcut 1=L1+R1:Ctrl+Shift+S
  python config_tool.py shortcut 4=Home:F13
  python config_tool.py shortcut 5=Home*2:F14
  python config_tool.py shortcut 2=L3+R3:VolumeUp
  python config_tool.py shortcut 3=Create+Options:bt_disconnect
  python config_tool.py shortcut 6=Create+Options*2:bt_disconnect
  python config_tool.py shortcut 9=Mute*2:Play
  python config_tool.py shortcut 1=off
  python config_tool.py fields
"""
import argparse
import struct
import sys
import time


def _load_hid():
    try:
        import hid
    except ImportError:
        sys.exit("Missing dependency. Install with:  pip install hidapi")
    return hid


VID = 0x054C
PIDS = (0x0CE6, 0x0DF2)  # DualSense, DualSense Edge
HID_USAGE_PAGE_GENERIC_DESKTOP = 0x01
HID_USAGE_GAMEPAD = 0x05

REPORT_SET = 0xF6        # SET_REPORT: write/save config
REPORT_GET_CONFIG = 0xF7  # GET_REPORT: read Config_body
REPORT_GET_VERSION = 0xF8  # GET_REPORT: firmware version string
REPORT_REMAP = 0xFA       # GET/SET_REPORT: button remap table
REPORT_SHORTCUT = 0xFB    # GET/SET_REPORT: shortcut slots

FUNC_UPDATE = 0x01       # update config in RAM
FUNC_SAVE = 0x02         # persist to flash
FUNC_RECONNECT = 0x03    # reconnect tinyusb device

SET_DATA_LEN = 63        # data bytes after the report id (descriptor report count 0x3F)
FEATURE_REPORT_LEN = SET_DATA_LEN + 1  # report id + descriptor report count

# On macOS, back-to-back IOHID feature reports can return before TinyUSB has
# dispatched the preceding SET_REPORT callback.  Besides making a following
# GET_REPORT return stale data, this can also make the separate save command
# overtake the in-RAM update.  Keep the protocol ordered with a small settling
# interval and an explicit read barrier between update and save.
HID_SET_REPORT_DELAY = 0.05

CONFIG_VERSION = 5       # src/config.cpp CONFIG_VERSION (display only)

BUTTON_NAMES = (
    "DPadNorth",
    "DPadNorthEast",
    "DPadEast",
    "DPadSouthEast",
    "DPadSouth",
    "DPadSouthWest",
    "DPadWest",
    "DPadNorthWest",
    "Square",
    "Cross",
    "Circle",
    "Triangle",
    "L1",
    "R1",
    "L2",
    "R2",
    "Create",
    "Options",
    "L3",
    "R3",
    "Home",
    "Pad",
    "Mute",
    "LeftFunction",
    "RightFunction",
    "LeftPaddle",
    "RightPaddle",
    "Disable",
)
BUTTON_COUNT = len(BUTTON_NAMES)
BUTTON_REMAP_COUNT = BUTTON_COUNT
BUTTON_SOURCE_COUNT = BUTTON_COUNT - 1
SHORTCUT_COUNT = 9        # src/config.h BUTTON_SHORTCUT_COUNT
SHORTCUT_PAYLOAD_SIZE = 3
SHORTCUT_SIZE = 3 + SHORTCUT_PAYLOAD_SIZE + 1  # triggers/action + payload + flags
SHORTCUT_DISABLED = 0xFF
TRIGGER_TAP = 0xFD        # src/config.h SHORTCUT_TRIGGER_TAP
TRIGGER_DOUBLE_TAP = 0xFE # src/config.h SHORTCUT_TRIGGER_DOUBLE_TAP
SHORTCUT_FLAG_DOUBLE_TAP = 0x01 # src/config.h SHORTCUT_FLAG_DOUBLE_TAP (chords only)
SHORTCUT_FLAG_HOLD = 0x02
SHORTCUT_FLAG_MASK = SHORTCUT_FLAG_DOUBLE_TAP | SHORTCUT_FLAG_HOLD
SHORTCUT_ACTION_KEYBOARD = 0
SHORTCUT_ACTION_BT_DISCONNECT = 1
SHORTCUT_ACTION_CONSUMER = 2
DPAD_MAX = 7               # DPadNorthWest; the hat reports one direction at a time
KEY_USAGE_MAX = 0x73       # src/usb_descriptors.h SHORTCUT_KEY_USAGE_MAX
CONSUMER_USAGE_MAX = 0x2FF # src/usb_descriptors.h SHORTCUT_CONSUMER_USAGE_MAX
SHORTCUT_STORAGE_SIZE = SHORTCUT_COUNT * SHORTCUT_SIZE
if SHORTCUT_STORAGE_SIZE > SET_DATA_LEN:
    raise RuntimeError(
        f"Shortcut slots need {SHORTCUT_STORAGE_SIZE} bytes, but report "
        f"0x{REPORT_SHORTCUT:02X} carries {SET_DATA_LEN}."
    )

MODIFIER_NAMES = {
    "ctrl": 0x01, "control": 0x01, "leftctrl": 0x01, "leftcontrol": 0x01,
    "shift": 0x02, "leftshift": 0x02,
    "alt": 0x04, "leftalt": 0x04,
    "gui": 0x08, "win": 0x08, "windows": 0x08, "command": 0x08, "meta": 0x08,
    "rightctrl": 0x10, "rightcontrol": 0x10,
    "rightshift": 0x20,
    "rightalt": 0x40,
    "rightgui": 0x80, "rightwin": 0x80, "rightcommand": 0x80,
}

KEY_NAMES = {chr(ord("a") + i): 0x04 + i for i in range(26)}
KEY_NAMES.update({str(i): 0x1D + i for i in range(1, 10)})
KEY_NAMES["0"] = 0x27
KEY_NAMES.update({f"f{i}": 0x39 + i for i in range(1, 13)})
KEY_NAMES.update({f"f{i}": 0x5B + i for i in range(13, 25)})
KEY_NAMES.update({
    "enter": 0x28, "return": 0x28, "esc": 0x29, "escape": 0x29,
    "backspace": 0x2A, "tab": 0x2B, "space": 0x2C,
    "minus": 0x2D, "equal": 0x2E, "leftbracket": 0x2F, "rightbracket": 0x30,
    "backslash": 0x31, "semicolon": 0x33, "apostrophe": 0x34, "grave": 0x35,
    "comma": 0x36, "period": 0x37, "slash": 0x38, "capslock": 0x39,
    "printscreen": 0x46, "prtscn": 0x46, "prtsc": 0x46, "scrolllock": 0x47, "pause": 0x48,
    "insert": 0x49, "home": 0x4A, "pageup": 0x4B, "delete": 0x4C,
    "end": 0x4D, "pagedown": 0x4E, "right": 0x4F, "left": 0x50,
    "down": 0x51, "up": 0x52, "numlock": 0x53, "menu": 0x65,
})
# Aliases follow their canonical name in KEY_NAMES, so keep the first name seen
# for each usage id -- otherwise "VolumeUp" would echo back as "Volup".
KEY_ID_TO_NAME = {}
for _name, _value in KEY_NAMES.items():
    KEY_ID_TO_NAME.setdefault(_value, _name.upper() if len(_name) == 1 else _name.title())

# Consumer page (0x0C) usages. Volume/mute/transport keys have Keyboard-page
# equivalents (0x7F..0x81) that Windows silently ignores, so they are a separate
# action delivered on the consumer HID interface.
CONSUMER_NAMES = {
    "mute": 0x00E2,
    "volumeup": 0x00E9, "volup": 0x00E9,
    "volumedown": 0x00EA, "voldown": 0x00EA,
    "playpause": 0x00CD, "play": 0x00CD,
    "nexttrack": 0x00B5, "next": 0x00B5,
    "prevtrack": 0x00B6, "previoustrack": 0x00B6, "prev": 0x00B6,
    "stop": 0x00B7,
    "brightnessup": 0x006F, "brightnessdown": 0x0070,
}
CONSUMER_ID_TO_NAME = {}
for _name, _value in CONSUMER_NAMES.items():
    CONSUMER_ID_TO_NAME.setdefault(_value, _name.title())


def normalize_button_name(name):
    return "".join(c for c in name.lower() if c.isalnum())


BUTTON_NAME_TO_ID = {
    normalize_button_name(name): index
    for index, name in enumerate(BUTTON_NAMES)
}
BUTTON_NAME_TO_ID.update({
    "up": BUTTON_NAME_TO_ID["dpadnorth"],
    "north": BUTTON_NAME_TO_ID["dpadnorth"],
    "upright": BUTTON_NAME_TO_ID["dpadnortheast"],
    "northeast": BUTTON_NAME_TO_ID["dpadnortheast"],
    "ne": BUTTON_NAME_TO_ID["dpadnortheast"],
    "right": BUTTON_NAME_TO_ID["dpadeast"],
    "east": BUTTON_NAME_TO_ID["dpadeast"],
    "downright": BUTTON_NAME_TO_ID["dpadsoutheast"],
    "southeast": BUTTON_NAME_TO_ID["dpadsoutheast"],
    "se": BUTTON_NAME_TO_ID["dpadsoutheast"],
    "down": BUTTON_NAME_TO_ID["dpadsouth"],
    "south": BUTTON_NAME_TO_ID["dpadsouth"],
    "downleft": BUTTON_NAME_TO_ID["dpadsouthwest"],
    "southwest": BUTTON_NAME_TO_ID["dpadsouthwest"],
    "sw": BUTTON_NAME_TO_ID["dpadsouthwest"],
    "left": BUTTON_NAME_TO_ID["dpadwest"],
    "west": BUTTON_NAME_TO_ID["dpadwest"],
    "upleft": BUTTON_NAME_TO_ID["dpadnorthwest"],
    "northwest": BUTTON_NAME_TO_ID["dpadnorthwest"],
    "nw": BUTTON_NAME_TO_ID["dpadnorthwest"],
    "ps": BUTTON_NAME_TO_ID["home"],
    "touchpad": BUTTON_NAME_TO_ID["pad"],
    "off": BUTTON_NAME_TO_ID["disable"],
})

# These are CLI-only conveniences, not ButtonId enum members. Clearing a remap
# now writes the source button's own ID (an identity mapping) to the table.
CLEAR_REMAP_NAMES = {"nomap", "none", "default", "self"}

# struct.pack/unpack codes per field kind.
KIND_TO_CODE = {"u8": "B", "float": "f"}

# FIELDS is the single source of truth for the packed Config_body layout
# (src/config.h). To add/remove/reorder a field, edit ONLY this table -- the
# binary format (STRUCT_FMT) is derived from the 'kind' column below.
# name, kind, validator(value)->bool, help. Order MUST match Config_body.
FIELDS = [
    ("config_version",     "u8",    lambda v: True,              "config schema version (read-only, managed by firmware)"),
    ("haptics_gain",       "float", lambda v: 1.0 <= v <= 2.0,   "[1.0, 2.0]"),
    ("speaker_volume",     "u8",    lambda v: 0 <= v <= 127,     "[0, 127]"),
    ("headset_volume",     "u8",    lambda v: 0 <= v <= 127,     "[0, 127]"),
    ("speaker_gain",       "u8",    lambda v: 0 <= v <= 7,       "[0, 7]"),
    ("inactive_time",      "u8",    lambda v: 0 <= v <= 60,      "[0, 60] minutes (0 disable)"),
    ("disable_pico_led",   "u8",    lambda v: v in (0, 1),       "0/1"),
    ("polling_rate_mode",  "u8",    lambda v: v in (0, 1, 2),    "0:250Hz 1:500Hz 2:real-time"),
    ("audio_buffer_length","u8",    lambda v: 16 <= v <= 128,    "[16, 128]"),
    ("controller_mode",    "u8",    lambda v: v in (0, 1, 2),    "0:DS5 1:DSE 2:Auto"),
    ("enable_usb_sn",      "u8",    lambda v: v in (0, 1),       "0/1 (USB serial number)"),
    ("enable_keyboard",    "u8",    lambda v: v in (0, 1),       "0/1 (USB keyboard interface)"),
    ("mic_select",         "u8",    lambda v: v in (0, 1, 2, 3), "0:auto 1:builtin 2:headphone 3:disable"),
    ("speaker_select",     "u8",    lambda v: v in (0, 1, 2, 3), "0:auto 1:builtin 2:headphone 3:disable"),
    ("enable_wake",        "u8",    lambda v: v in (0, 1),       "0/1 (wake host on PS press)"),
    ("trigger_reduce",     "u8",    lambda v: 0 <= v <= 10,      "[0, 10] (0: auto)"),
    ("lock_volume",        "u8",    lambda v: v in (0, 1),       "0/1 (ignore the volume change from SetStateData(game or software))"),
    ("status_gpio_pin",    "u8",    lambda v: 0 <= v <= 255,     "GPIO number (255 disables; firmware rejects board-reserved pins)"),
    ("status_gpio_mode",   "u8",    lambda v: v in (0, 1),       "0:pull high 1:200ms button pulse"),
]
FIELD_NAMES = [f[0] for f in FIELDS]
# Little-endian, no padding -- matches __attribute__((packed)) Config_body.
STRUCT_FMT = "<" + "".join(KIND_TO_CODE[f[1]] for f in FIELDS)
BODY_SIZE = struct.calcsize(STRUCT_FMT)
if BODY_SIZE > SET_DATA_LEN - 1:
    raise RuntimeError(
        f"Config_body is {BODY_SIZE} bytes, but the update report only has "
        f"{SET_DATA_LEN - 1} bytes available."
    )


def unpack_config(body):
    unpacked = iter(struct.unpack(STRUCT_FMT, body))
    cfg = {}
    for name, kind, _validator, _helptext in FIELDS:
        cfg[name] = next(unpacked)
    return cfg


def pack_config(cfg):
    values = []
    for name, kind, validator, _helptext in FIELDS:
        value = cfg[name]
        if not validator(value):
            raise ValueError(f"Invalid value for {name}: {value!r}")
        values.append(value)
    return struct.pack(STRUCT_FMT, *values)


def is_gamepad_hid(devinfo):
    return (devinfo.get("usage_page") == HID_USAGE_PAGE_GENERIC_DESKTOP and
            devinfo.get("usage") == HID_USAGE_GAMEPAD)


def fmt_hex(value):
    if value is None:
        return "?"
    return f"0x{int(value):04X}"


def describe_hid(devinfo):
    return (
        f"pid={fmt_hex(devinfo.get('product_id'))}, "
        f"interface={devinfo.get('interface_number', '?')}, "
        f"usage_page={fmt_hex(devinfo.get('usage_page'))}, "
        f"usage={fmt_hex(devinfo.get('usage'))}, "
        f"product={devinfo.get('product_string') or '?'}"
    )


def open_device():
    hid = _load_hid()
    cand = [d for d in hid.enumerate(VID) if d["product_id"] in PIDS]
    if not cand:
        sys.exit("No DualSense / ds5dongle found (VID 054C, PID 0CE6/0DF2). "
                 "Close Steam/DSX if they're holding the device.")
    gamepads = [d for d in cand if is_gamepad_hid(d)]
    if not gamepads:
        detail = "\n".join(f"  {describe_hid(d)}" for d in cand)
        sys.exit("Found DualSense / ds5dongle HID device(s), but none were the Game Pad interface "
                 "(usage_page=0x0001, usage=0x0005). Wake adds a keyboard HID; "
                 "this tool only opens the gamepad.\n" + detail)
    dev = hid.device()
    dev.open_path(gamepads[0]["path"])
    return dev


def read_config(dev):
    # Windows hidapi expects the buffer to match the HID feature report length.
    # The config body is shorter than the descriptor report count, so read the
    # full report and unpack only Config_body.
    try:
        data = dev.get_feature_report(REPORT_GET_CONFIG, FEATURE_REPORT_LEN)
    except OSError as exc:
        sys.exit(f"Failed reading config report 0x{REPORT_GET_CONFIG:02X}: {exc}")
    if not data:
        sys.exit("Empty response reading config (report 0xF7). Is the firmware current?")
    body = bytes(data[1:1 + BODY_SIZE]) if data[0] == REPORT_GET_CONFIG else bytes(data[:BODY_SIZE])
    if len(body) < BODY_SIZE:
        sys.exit(f"Short config read: got {len(body)} bytes, expected {BODY_SIZE}.")
    return unpack_config(body)

def read_version(dev):
    try:
        data = dev.get_feature_report(REPORT_GET_VERSION, FEATURE_REPORT_LEN)
    except OSError:
        return ""
    raw = bytes(data[1:]) if data and data[0] == REPORT_GET_VERSION else bytes(data or b"")
    return raw.split(b"\x00", 1)[0].decode("ascii", "replace").strip()


def read_area(dev, report_id, size, what):
    try:
        data = dev.get_feature_report(report_id, FEATURE_REPORT_LEN)
    except OSError as exc:
        sys.exit(f"Failed reading {what} (report 0x{report_id:02X}): {exc}")
    if not data:
        sys.exit(f"Empty response reading {what} (report 0x{report_id:02X}). "
                 "Is the firmware current?")
    raw = bytes(data[1:]) if data[0] == report_id else bytes(data)
    if len(raw) < size:
        sys.exit(f"Short {what} read: got {len(raw)} bytes, expected {size}.")
    return bytearray(raw[:size])


def read_remap(dev):
    return read_area(dev, REPORT_REMAP, BUTTON_REMAP_COUNT, "button remaps")


def read_shortcuts(dev):
    return read_area(dev, REPORT_SHORTCUT, SHORTCUT_STORAGE_SIZE, "shortcut slots")

def send_feature_report(dev, data, operation, report_id=REPORT_SET):
    report = bytes([report_id]) + data
    try:
        sent = dev.send_feature_report(report)
    except OSError as exc:
        sys.exit(f"Failed {operation}: {exc}")
    if sent is not None and sent != len(report):
        sys.exit(
            f"Failed {operation}: wrote {sent} of {len(report)} report bytes."
        )
    time.sleep(HID_SET_REPORT_DELAY)


def write_config(dev, cfg, save):
    body = pack_config(cfg)
    # [report id][funcid 0x01][body...] padded to SET_DATA_LEN data bytes.
    data = bytes([FUNC_UPDATE]) + body
    data = data[:SET_DATA_LEN].ljust(SET_DATA_LEN, b"\x00")
    send_feature_report(dev, data, "updating config")

    # This read is also a USB control-transfer barrier: do not submit FUNC_SAVE
    # until the firmware has applied FUNC_UPDATE.  It is the authoritative
    # value to display because firmware validation may clamp some fields.
    new_cfg = read_config(dev)

    if save:
        save_data = bytes([FUNC_SAVE]).ljust(SET_DATA_LEN, b"\x00")
        send_feature_report(dev, save_data, "saving config to flash")
        # Confirm that the device still answers after the flash operation.
        new_cfg = read_config(dev)
    return new_cfg


def write_remap(dev, remap, save):
    send_feature_report(
        dev, bytes(remap), "updating button remaps", report_id=REPORT_REMAP
    )
    save_button_sector(dev, save)
    return read_remap(dev)


def write_shortcuts(dev, shortcuts, save):
    send_feature_report(
        dev, bytes(shortcuts), "updating shortcut slots", report_id=REPORT_SHORTCUT
    )
    save_button_sector(dev, save)
    return read_shortcuts(dev)


def save_button_sector(dev, save):
    if not save:
        return
    save_data = bytes([FUNC_SAVE]).ljust(SET_DATA_LEN, b"\x00")
    send_feature_report(dev, save_data, "saving Button settings to flash")


def fmt_value(name, value):
    if name == "haptics_gain":
        return f"{value:.3f}"
    return str(value)


def print_config(cfg):
    width = max(len(n) for n in FIELD_NAMES)
    for name, _kind, _ok, helptext in FIELDS:
        print(f"  {name:<{width}} = {fmt_value(name, cfg[name]):<8}  # {helptext}")


def parse_assignment(token):
    if "=" not in token:
        sys.exit(f"Bad assignment '{token}', expected name=value.")
    name, raw = token.split("=", 1)
    name = name.strip()
    if name not in FIELD_NAMES:
        sys.exit(f"Unknown field '{name}'. Run 'config_tool.py fields' to list them.")
    if name == "config_version":
        sys.exit("config_version is managed by the firmware and cannot be set.")
    kind = dict((f[0], f[1]) for f in FIELDS)[name]
    validator = dict((f[0], f[2]) for f in FIELDS)[name]
    try:
        value = float(raw) if kind == "float" else int(raw, 0)
    except ValueError:
        sys.exit(f"Bad value '{raw}' for {name}.")
    if not validator(value):
        helptext = dict((f[0], f[3]) for f in FIELDS)[name]
        sys.exit(f"Value {raw} out of range for {name} (expected {helptext}).")
    return name, value


def cmd_fields(_args):
    width = max(len(n) for n in FIELD_NAMES)
    print(f"Config_body ({BODY_SIZE} bytes, schema version {CONFIG_VERSION}):")
    for name, kind, _ok, helptext in FIELDS:
        ro = " (read-only)" if name == "config_version" else ""
        print(f"  {name:<{width}} {kind:<6} {helptext}{ro}")


def cmd_get(_args):
    dev = open_device()
    try:
        version = read_version(dev)
        cfg = read_config(dev)
        remap = read_remap(dev)
        shortcuts = read_shortcuts(dev)
    finally:
        dev.close()
    if version:
        print(f"Firmware: {version}")
    print("Config:")
    print_config(cfg)
    print("Button remaps:")
    print_remaps(remap)
    print("Shortcut slots:")
    print_shortcuts(shortcuts)


def cmd_set(args):
    updates = dict(parse_assignment(t) for t in args.assignments)
    if not updates:
        sys.exit("Nothing to set. Pass one or more name=value pairs.")
    dev = open_device()
    try:
        cfg = read_config(dev)
        cfg.update(updates)
        new_cfg = write_config(dev, cfg, save=not args.no_save)
    finally:
        dev.close()
    print("Updated:" + ("" if args.no_save else " (saved to flash)"))
    for name in updates:
        print(f"  {name} -> {fmt_value(name, new_cfg[name])}")
    # Firmware clamps invalid values; surface any that were adjusted.
    for name, want in updates.items():
        got = new_cfg[name]
        adjusted = abs(got - want) > 1e-6 if isinstance(want, float) else got != want
        if adjusted:
            print(f"  note: {name} was clamped by firmware to {fmt_value(name, got)}")


def parse_button(raw, *, source):
    key = normalize_button_name(raw)
    if key not in BUTTON_NAME_TO_ID:
        valid_names = BUTTON_NAMES[:-1] if source else BUTTON_NAMES + ("NoMap",)
        valid = ", ".join(valid_names)
        sys.exit(f"Unknown button '{raw}'. Valid buttons: {valid}.")
    button_id = BUTTON_NAME_TO_ID[key]
    if source and button_id == BUTTON_COUNT - 1:
        sys.exit(f"'{raw}' cannot be used as a source button.")
    return button_id


def button_name(button_id):
    return BUTTON_NAMES[button_id] if 0 <= button_id < BUTTON_COUNT else f"Invalid({button_id})"


def parse_remap_assignment(token):
    if "=" not in token:
        sys.exit(f"Bad remap '{token}', expected source=target.")
    source_raw, target_raw = token.split("=", 1)
    source_id = parse_button(source_raw.strip(), source=True)
    target_key = normalize_button_name(target_raw.strip())
    target_id = (source_id if target_key in CLEAR_REMAP_NAMES else
                 parse_button(target_raw.strip(), source=False))
    return source_id, target_id


def print_remaps(remap, indent="  "):
    for source in range(BUTTON_SOURCE_COUNT):
        target = remap[source]
        print(f"{indent}{BUTTON_NAMES[source]:<15} -> {button_name(target)}")


def cmd_remap(args):
    if args.reset and args.assignments:
        sys.exit("--reset cannot be combined with source=target assignments.")

    updates = dict(parse_remap_assignment(token) for token in args.assignments)
    dev = open_device()
    try:
        remap = read_remap(dev)
        if not updates and not args.reset:
            print("Button remaps:")
            print_remaps(remap)
            return
        if args.reset:
            remap[:BUTTON_REMAP_COUNT] = range(BUTTON_REMAP_COUNT)
        else:
            for source, target in updates.items():
                remap[source] = target
        new_remap = write_remap(dev, remap, save=not args.no_save)
    finally:
        dev.close()

    if args.reset:
        expected = bytes(range(BUTTON_REMAP_COUNT))
        if new_remap[:BUTTON_REMAP_COUNT] != expected:
            sys.exit("Read-back verification failed while resetting button remaps.")
        print("Reset all button remaps to their defaults:" +
              ("" if args.no_save else " (saved to flash)"))
        print_remaps(new_remap)
        return

    for source in updates:
        target = new_remap[source]
        if target != updates[source]:
            sys.exit(
                f"Read-back verification failed for {BUTTON_NAMES[source]}: "
                f"requested {button_name(updates[source])}, got {button_name(target)}."
            )

    print("Updated button remaps:" + ("" if args.no_save else " (saved to flash)"))
    for source in updates:
        target = new_remap[source]
        print(f"  {BUTTON_NAMES[source]:<15} -> {button_name(target)}")


def shortcut_offset(slot):
    return slot * SHORTCUT_SIZE


def unpack_shortcut(button, slot):
    offset = shortcut_offset(slot)
    return (button[offset], button[offset + 1], button[offset + 2],
            list(button[offset + 3:offset + 3 + SHORTCUT_PAYLOAD_SIZE]),
            button[offset + SHORTCUT_SIZE - 1])


def pack_shortcut(button, slot, shortcut):
    trigger_a, trigger_b, action, payload, flags = shortcut
    offset = shortcut_offset(slot)
    button[offset:offset + SHORTCUT_SIZE] = bytes(
        [trigger_a, trigger_b, action] + payload[:SHORTCUT_PAYLOAD_SIZE] + [flags]
    )


def disabled_shortcut():
    return (SHORTCUT_DISABLED, SHORTCUT_DISABLED,
            SHORTCUT_ACTION_KEYBOARD, [0] * SHORTCUT_PAYLOAD_SIZE, 0)


def key_name(key_id):
    return KEY_ID_TO_NAME.get(key_id, f"0x{key_id:02X}")


def consumer_name(usage):
    return CONSUMER_ID_TO_NAME.get(usage, f"consumer 0x{usage:04X}")


def format_keyboard_chord(modifiers, keys):
    modifier_order = (
        (0x01, "Ctrl"), (0x02, "Shift"), (0x04, "Alt"), (0x08, "Win"),
        (0x10, "RightCtrl"), (0x20, "RightShift"),
        (0x40, "RightAlt"), (0x80, "RightWin"),
    )
    parts = [name for bit, name in modifier_order if modifiers & bit]
    parts.extend(key_name(key) for key in keys if key)
    return "+".join(parts) if parts else "(empty)"


def shortcut_slot_valid(shortcut):
    """Mirror of shortcut_slot_valid() in src/config.cpp."""
    trigger_a, trigger_b, action, payload, flags = shortcut
    if trigger_a >= BUTTON_SOURCE_COUNT:
        return False
    if flags & ~SHORTCUT_FLAG_MASK:
        return False
    if flags & SHORTCUT_FLAG_HOLD and (
        action != SHORTCUT_ACTION_KEYBOARD or flags & SHORTCUT_FLAG_DOUBLE_TAP
        or trigger_b == TRIGGER_DOUBLE_TAP
    ):
        return False
    if trigger_b in (TRIGGER_TAP, TRIGGER_DOUBLE_TAP):
        if flags & SHORTCUT_FLAG_DOUBLE_TAP:
            return False
    else:
        if trigger_b >= BUTTON_SOURCE_COUNT or trigger_a == trigger_b:
            return False
        if trigger_a <= DPAD_MAX and trigger_b <= DPAD_MAX:
            return False
    if action == SHORTCUT_ACTION_KEYBOARD:
        return payload[1] <= KEY_USAGE_MAX and (payload[0] != 0 or payload[1] != 0)
    if action == SHORTCUT_ACTION_CONSUMER:
        usage = payload[0] | (payload[1] << 8)
        return 0 < usage <= CONSUMER_USAGE_MAX
    return action == SHORTCUT_ACTION_BT_DISCONNECT


def print_shortcuts(button, indent="  "):
    for slot in range(SHORTCUT_COUNT):
        shortcut = unpack_shortcut(button, slot)
        if not shortcut_slot_valid(shortcut):
            print(f"{indent}{slot + 1}: off")
            continue
        trigger_a, trigger_b, action, payload, flags = shortcut
        if trigger_b == TRIGGER_TAP:
            triggers = button_name(trigger_a)
        elif trigger_b == TRIGGER_DOUBLE_TAP:
            triggers = f"{button_name(trigger_a)}*2"
        else:
            triggers = f"{button_name(trigger_a)}+{button_name(trigger_b)}"
            if flags & SHORTCUT_FLAG_DOUBLE_TAP:
                triggers += "*2"
        if flags & SHORTCUT_FLAG_HOLD:
            triggers += "@hold"
        if action == SHORTCUT_ACTION_BT_DISCONNECT:
            output = "bt_disconnect"
        elif action == SHORTCUT_ACTION_CONSUMER:
            output = consumer_name(payload[0] | (payload[1] << 8))
        else:
            output = format_keyboard_chord(payload[0], payload[1:2])
        print(f"{indent}{slot + 1}: {triggers} -> {output}")


def parse_key(raw):
    normalized = normalize_button_name(raw)
    if normalized in KEY_NAMES:
        return KEY_NAMES[normalized]
    try:
        value = int(raw, 0)
    except ValueError:
        sys.exit(f"Unknown keyboard key '{raw}'. Use a key name or HID usage ID (for example 0x16).")
    if not 0x04 <= value <= KEY_USAGE_MAX:
        sys.exit(f"Keyboard HID usage '{raw}' is outside 0x04..0x{KEY_USAGE_MAX:02X}.")
    return value


def parse_shortcut_assignment(token):
    if "=" not in token:
        sys.exit(f"Bad shortcut '{token}', expected slot=Button[+Button][*2]:output.")
    slot_raw, definition = token.split("=", 1)
    try:
        slot = int(slot_raw, 10) - 1
    except ValueError:
        sys.exit(f"Bad shortcut slot '{slot_raw}', expected 1..{SHORTCUT_COUNT}.")
    if not 0 <= slot < SHORTCUT_COUNT:
        sys.exit(f"Shortcut slot must be in 1..{SHORTCUT_COUNT}.")
    if definition.strip().lower() in ("off", "disable", "none"):
        return slot, disabled_shortcut()
    if ":" not in definition:
        sys.exit(f"Bad shortcut '{token}', missing ':' between controller trigger and output.")
    trigger_raw, output_raw = definition.split(":", 1)
    hold = trigger_raw.strip().lower().endswith("@hold")
    if hold:
        trigger_raw = trigger_raw.strip()[:-5]
        if "*" in trigger_raw:
            sys.exit("@hold is separate from single/double tap syntax.")
    triggers = [part.strip() for part in trigger_raw.split("+") if part.strip()]
    flags = SHORTCUT_FLAG_HOLD if hold else 0
    if len(triggers) == 1:
        # A lone button is a tap trigger; a trailing "*2" asks for a double tap.
        name = triggers[0]
        taps = 1
        if "*" in name:
            name, _, taps_raw = name.partition("*")
            taps_raw = taps_raw.strip()
            if taps_raw not in ("1", "2"):
                sys.exit("A tap trigger supports '*1' (single) or '*2' (double) only.")
            taps = int(taps_raw)
        trigger_ids = [parse_button(name.strip(), source=True),
                       TRIGGER_TAP if taps == 1 else TRIGGER_DOUBLE_TAP]
    elif len(triggers) == 2:
        # A trailing "*2" on the second button asks for a double-tapped chord.
        if "*" in triggers[0]:
            sys.exit("Put the '*2' at the end of the chord, for example 'Create+Options*2'.")
        if "*" in triggers[1]:
            name, _, taps_raw = triggers[1].partition("*")
            if taps_raw.strip() not in ("1", "2"):
                sys.exit("A chord supports '*1' (hold) or '*2' (double tap) only.")
            if taps_raw.strip() == "2":
                flags |= SHORTCUT_FLAG_DOUBLE_TAP
            triggers[1] = name.strip()
        trigger_ids = [parse_button(part, source=True) for part in triggers]
        if trigger_ids[0] == trigger_ids[1]:
            sys.exit("The two controller buttons in a shortcut must be different.")
        if trigger_ids[0] <= DPAD_MAX and trigger_ids[1] <= DPAD_MAX:
            sys.exit("A shortcut cannot use two DPad directions: the DPad reports a single "
                     "direction at a time, so the chord could never fire.")
    else:
        sys.exit("A shortcut needs one button (tap trigger) or two buttons (chord).")

    action_name = normalize_button_name(output_raw)
    if hold and (action_name in CONSUMER_NAMES or action_name in ("btdisconnect", "disconnect")):
        sys.exit("@hold supports keyboard keys and keyboard shortcuts only.")
    if action_name in ("btdisconnect", "disconnect"):
        return slot, (trigger_ids[0], trigger_ids[1],
                      SHORTCUT_ACTION_BT_DISCONNECT, [0] * SHORTCUT_PAYLOAD_SIZE, flags)
    if action_name in CONSUMER_NAMES:
        # Consumer usages are standalone; the HID consumer report carries no modifiers.
        usage = CONSUMER_NAMES[action_name]
        return slot, (trigger_ids[0], trigger_ids[1], SHORTCUT_ACTION_CONSUMER,
                      [usage & 0xFF, usage >> 8, 0], flags)

    modifiers = 0
    keys = []
    for part in (part.strip() for part in output_raw.split("+") if part.strip()):
        normalized = normalize_button_name(part)
        if normalized in MODIFIER_NAMES:
            modifiers |= MODIFIER_NAMES[normalized]
        else:
            keys.append(parse_key(part))
    if len(keys) > 1:
        sys.exit("A keyboard shortcut supports at most one non-modifier key.")
    if not modifiers and not keys:
        sys.exit("The keyboard chord cannot be empty.")
    key = keys[0] if keys else 0
    return slot, (trigger_ids[0], trigger_ids[1], SHORTCUT_ACTION_KEYBOARD,
                  [modifiers, key, 0], flags)


def cmd_shortcut(args):
    if args.reset and args.assignments:
        sys.exit("--reset cannot be combined with shortcut assignments.")
    if args.reset:
        updates = {slot: disabled_shortcut() for slot in range(SHORTCUT_COUNT)}
    else:
        updates = dict(parse_shortcut_assignment(token) for token in args.assignments)
    dev = open_device()
    try:
        shortcuts = read_shortcuts(dev)
        if not updates:
            print("Shortcut slots:")
            print_shortcuts(shortcuts)
            return
        for slot, shortcut in updates.items():
            pack_shortcut(shortcuts, slot, shortcut)
        new_shortcuts = write_shortcuts(dev, shortcuts, save=not args.no_save)
        for slot, shortcut in updates.items():
            if unpack_shortcut(new_shortcuts, slot) != shortcut:
                sys.exit(f"Read-back verification failed for shortcut slot {slot + 1}.")
    finally:
        dev.close()

    print("Updated shortcuts:" + ("" if args.no_save else " (saved to Button Sector)"))
    print_shortcuts(new_shortcuts)


def main():
    parser = argparse.ArgumentParser(description="Read and modify ds5dongle config over USB HID.")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("get", help="read and print the current config").set_defaults(func=cmd_get)
    sub.add_parser("fields", help="list configurable fields and ranges").set_defaults(func=cmd_fields)

    p_set = sub.add_parser("set", help="set one or more fields (name=value ...)")
    p_set.add_argument("assignments", nargs="+", metavar="name=value")
    p_set.add_argument("--no-save", action="store_true",
                       help="update RAM only; do not persist to flash")
    p_set.set_defaults(func=cmd_set)

    p_remap = sub.add_parser(
        "remap",
        help="view or set button remaps (source=target; nomap restores identity, disable blocks)",
    )
    p_remap.add_argument("assignments", nargs="*", metavar="source=target")
    p_remap.add_argument("--reset", action="store_true",
                         help="reset all button remaps to their identity defaults")
    p_remap.add_argument("--no-save", action="store_true",
                         help="update RAM only; do not persist to flash")
    p_remap.set_defaults(func=cmd_remap)

    p_shortcut = sub.add_parser(
        "shortcut",
        help="view or set up to 9 controller chord/tap action slots",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""Examples:
  python tools/config_tool.py shortcut
  python tools/config_tool.py shortcut 1=PS+Create:Win+PrintScreen
  python tools/config_tool.py shortcut 2=PS+UP:VolumeUp
  python tools/config_tool.py shortcut 3=PS+DOWN:VolumeDown
  python tools/config_tool.py shortcut 4=PS:Win+G
  python tools/config_tool.py shortcut 6=PS*2:Win+Tab
  python tools/config_tool.py shortcut "1=Cross@hold:Space"
  python tools/config_tool.py shortcut "2=L1@hold:Ctrl+C"
  python tools/config_tool.py shortcut 1=off""",
    )
    p_shortcut.add_argument(
        "assignments", nargs="*", metavar="slot=Button[+Button|*2]:output",
    )
    p_shortcut.add_argument("--no-save", action="store_true",
                            help="update RAM only; do not persist to flash")
    p_shortcut.add_argument("--reset", action="store_true",
                            help="turn off all shortcut mappings")
    p_shortcut.set_defaults(func=cmd_shortcut)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
