#!/usr/bin/env python3
"""
Read and modify the ds5dongle configuration over USB HID, without reflashing.

Protocol (see src/cmd.cpp / src/config.h):
  GET feature report 0xF7 -> Config_body bytes, optionally followed by BF extension
  GET feature report 0xF8 -> firmware version string
  SET feature report 0xF6:
      funcid 0x01 + body   -> update config in RAM (firmware clamps invalid values)
      funcid 0x02          -> persist config to flash
      funcid 0x03          -> reconnect the USB device
      funcid 0x05 + BF flag -> update battery feedback in RAM

Config_body is a packed struct; this tool derives the binary layout from FIELDS.

Requires: pip install hidapi

Examples:
  python config_tool.py get
  python config_tool.py set speaker_volume=90 enable_wake=1
  python config_tool.py set haptics_gain=1.5 --no-save
  python config_tool.py reconnect
  python config_tool.py fields
  python config_tool.py battery-feedback get
  python config_tool.py battery-feedback on --no-save
"""
import argparse
import struct
import sys
import time
from typing import NamedTuple


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

FUNC_UPDATE = 0x01       # update config in RAM
FUNC_SAVE = 0x02         # persist to flash
FUNC_RECONNECT = 0x03    # reconnect tinyusb device
FUNC_BATTERY_FEEDBACK = 0x05

SET_DATA_LEN = 63        # data bytes after the report id (descriptor report count 0x3F)
FEATURE_REPORT_LEN = SET_DATA_LEN + 1  # report id + descriptor report count

CONFIG_VERSION = 5       # src/config.cpp CONFIG_VERSION (display only)

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
    ("polling_rate_mode",  "u8",    lambda v: v in (0, 1, 2, 3), "0:250Hz 1:500Hz 2:real-time 3:1000Hz fixed (USB reconnect required)"),
    ("audio_buffer_length","u8",    lambda v: 16 <= v <= 128,    "[16, 128]"),
    ("controller_mode",    "u8",    lambda v: v in (0, 1, 2),    "0:DS5 1:DSE 2:Auto"),
    ("enable_usb_sn",      "u8",    lambda v: v in (0, 1),       "0/1 (USB serial number)"),
    ("ps_shortcut_enabled","u8",    lambda v: v in (0, 1),       "0/1 (Xbox Game Bar via HID keyboard)"),
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
BATTERY_EXTENSION_MARKER = b"BF"
BATTERY_EXTENSION_VERSION = 1
BATTERY_EXTENSION_SIZE = 5
READBACK_TIMEOUT_S = 1.0
READBACK_INTERVAL_S = 0.02


class BatteryExtension(NamedTuple):
    enabled: bool
    save_status: int


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


def _read_f7(dev, context):
    # Windows hidapi expects the buffer to match the HID feature report length.
    # Decode the base body and optional extension from the same response when
    # they form one logical snapshot.
    try:
        data = bytes(dev.get_feature_report(REPORT_GET_CONFIG, FEATURE_REPORT_LEN) or b"")
    except OSError as exc:
        sys.exit(f"Failed reading {context} report 0xF7: {exc}")
    if not data:
        sys.exit(f"Empty response reading {context} (report 0xF7). Is the firmware current?")
    return data


def _decode_config(data):
    body = bytes(data[1:1 + BODY_SIZE]) if data[0] == REPORT_GET_CONFIG else bytes(data[:BODY_SIZE])
    if len(body) < BODY_SIZE:
        sys.exit(f"Short config read: got {len(body)} bytes, expected {BODY_SIZE}.")
    values = struct.unpack(STRUCT_FMT, body)
    return dict(zip(FIELD_NAMES, values))


def read_config(dev):
    return _decode_config(_read_f7(dev, "config"))


def _decode_battery_extension(data):
    """Return None for old firmware, otherwise the independently versioned F7 tail."""
    body_start = 1 if data[0] == REPORT_GET_CONFIG else 0
    if len(data) < body_start + BODY_SIZE or data[body_start] != CONFIG_VERSION:
        sys.exit("Invalid F7 base config: short body or unsupported schema version.")
    tail_start = body_start + BODY_SIZE
    if data[tail_start:tail_start + 1] == b"B" and data[tail_start:tail_start + 2] != BATTERY_EXTENSION_MARKER:
        sys.exit("Invalid battery extension: malformed BF marker.")
    if len(data) < tail_start + 2 or data[tail_start:tail_start + 2] != BATTERY_EXTENSION_MARKER:
        return None
    if len(data) < tail_start + BATTERY_EXTENSION_SIZE:
        sys.exit("Invalid battery extension: truncated marked F7 tail.")
    version, flags, status = data[tail_start + 2:tail_start + BATTERY_EXTENSION_SIZE]
    if version != BATTERY_EXTENSION_VERSION or flags & ~1 or status not in (0, 1, 2):
        sys.exit("Invalid battery extension: unsupported version, flags, or save status.")
    return BatteryExtension(bool(flags & 1), status)


def read_battery_extension(dev):
    return _decode_battery_extension(_read_f7(dev, "battery extension"))


def read_version(dev):
    try:
        data = dev.get_feature_report(REPORT_GET_VERSION, FEATURE_REPORT_LEN)
    except OSError:
        return ""
    raw = bytes(data[1:]) if data and data[0] == REPORT_GET_VERSION else bytes(data or b"")
    return raw.split(b"\x00", 1)[0].decode("ascii", "replace").strip()


def send_feature_report_checked(dev, report, context):
    try:
        written = dev.send_feature_report(report)
    except OSError as exc:
        sys.exit(f"{context} feature report write failed: {exc}")
    if written != len(report):
        sys.exit(f"{context} feature report write incomplete: {written}/{len(report)} bytes.")


def save_config(dev):
    report = bytes((REPORT_SET, FUNC_SAVE)).ljust(FEATURE_REPORT_LEN, b"\x00")
    send_feature_report_checked(dev, report, "Config")


def poll_readback(read, accepted, error, accepted_at_timeout=None):
    deadline = time.monotonic() + READBACK_TIMEOUT_S
    while True:
        value = read()
        if accepted(value):
            return value
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            if accepted_at_timeout is not None and accepted_at_timeout(value):
                return value
            sys.exit(error)
        time.sleep(min(READBACK_INTERVAL_S, remaining))


def config_updates_visible(cfg, updates, previous_cfg, allow_unchanged_disabled_pin=False):
    for name, want in updates.items():
        got = cfg[name]
        if name == "status_gpio_pin" and got == 255 and want != 255:
            # A transition to 255 shows the firmware clamped a board-reserved
            # pin. When the prior value was already 255, defer this ambiguous
            # result until the readback deadline in case a valid pin appears.
            if previous_cfg[name] != 255 or allow_unchanged_disabled_pin:
                continue
            return False
        if isinstance(want, float):
            if abs(got - want) > 1e-6:
                return False
        elif got != want:
            return False
    return True


def read_config_snapshot(dev):
    data = _read_f7(dev, "config")
    return _decode_config(data), _decode_battery_extension(data)


def write_config(dev, cfg):
    body = struct.pack(STRUCT_FMT, *[cfg[name] for name in FIELD_NAMES])
    # [report id][funcid 0x01][body...] padded to SET_DATA_LEN data bytes.
    data = bytes([FUNC_UPDATE]) + body
    data = data[:SET_DATA_LEN].ljust(SET_DATA_LEN, b"\x00")
    send_feature_report_checked(dev, bytes([REPORT_SET]) + data, "Config")


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
    finally:
        dev.close()
    if version:
        print(f"Firmware: {version}")
    print("Config:")
    print_config(cfg)


def cmd_set(args):
    updates = dict(parse_assignment(t) for t in args.assignments)
    if not updates:
        sys.exit("Nothing to set. Pass one or more name=value pairs.")
    dev = open_device()
    try:
        previous_cfg = read_config(dev)
        previous_polling_rate = previous_cfg["polling_rate_mode"]
        cfg = previous_cfg.copy()
        cfg.update(updates)
        write_config(dev, cfg)
        def update_visible(snapshot, allow_unchanged_disabled_pin=False):
            return config_updates_visible(snapshot[0], updates, previous_cfg, allow_unchanged_disabled_pin) and (
                snapshot[1] is None or snapshot[1].save_status == 0)

        new_cfg, applied_extension = poll_readback(
            lambda: read_config_snapshot(dev),
            update_visible,
            "Config RAM update readback did not confirm the requested values before timeout.",
            accepted_at_timeout=lambda snapshot: update_visible(snapshot, allow_unchanged_disabled_pin=True))
        ambiguous_pin = ("status_gpio_pin" in updates and updates["status_gpio_pin"] != 255 and
                         previous_cfg["status_gpio_pin"] == new_cfg["status_gpio_pin"] == 255)

        if args.no_save:
            save_label = " (RAM update unconfirmed; readback remains disabled)" if ambiguous_pin else " (RAM only)"
        else:
            save_config(dev)
            saved_cfg, saved_extension = poll_readback(
                lambda: read_config_snapshot(dev),
                lambda snapshot: snapshot[0] == new_cfg and (
                    (applied_extension is None and snapshot[1] is None) or
                    (applied_extension is not None and snapshot[1] is not None and
                     snapshot[1].enabled == applied_extension.enabled and snapshot[1].save_status == 1)),
                "Config save verification failed: base readback or flash status did not match before timeout.")
            if applied_extension is not None:
                save_label = (" (flash save verified; requested pin update unconfirmed)" if ambiguous_pin else
                              " (flash save verified)")
            else:
                save_label = (" (save requested; flash unverified; requested pin update unconfirmed)" if ambiguous_pin
                              else " (save requested; flash unverified by legacy firmware)")
    finally:
        dev.close()
    print(("Readback:" if ambiguous_pin else "Updated:") + save_label)
    for name in updates:
        print(f"  {name} -> {fmt_value(name, new_cfg[name])}")
    # Firmware clamps invalid values; surface any that were adjusted.
    for name, want in updates.items():
        got = new_cfg[name]
        adjusted = abs(got - want) > 1e-6 if isinstance(want, float) else got != want
        if adjusted:
            if name == "status_gpio_pin" and previous_cfg[name] == got == 255:
                print("  note: status_gpio_pin remains 255. Board-reserved pins are clamped to disabled; "
                      "this unchanged readback cannot confirm the RAM update.")
            else:
                print(f"  note: {name} was clamped by firmware to {fmt_value(name, got)}")
    if "polling_rate_mode" in updates and new_cfg["polling_rate_mode"] != previous_polling_rate:
        print("  note: USB interval changes after reconnect; run 'python tools/config_tool.py reconnect' or replug the dongle.")


def cmd_reconnect(_args):
    dev = open_device()
    try:
        report = bytes([REPORT_SET, FUNC_RECONNECT]).ljust(FEATURE_REPORT_LEN, b"\x00")
        dev.send_feature_report(report)
    finally:
        dev.close()
    print("USB reconnect requested.")


def cmd_battery_feedback(args):
    dev = open_device()
    try:
        extension = read_battery_extension(dev)
        if extension is None:
            sys.exit("Battery feedback is not supported by this firmware.")

        if args.action == "get":
            labels = {0: "RAM state, flash unconfirmed", 1: "flash save verified", 2: "flash save verification failed"}
            print(f"Battery feedback: {'on' if extension.enabled else 'off'} ({labels[extension.save_status]}).")
            return

        enabled = args.action == "on"
        report = bytes((REPORT_SET, FUNC_BATTERY_FEEDBACK, 0x42, 0x46,
                        BATTERY_EXTENSION_VERSION, int(enabled))).ljust(FEATURE_REPORT_LEN, b"\x00")
        send_feature_report_checked(dev, report, "Battery feedback")
        poll_readback(
            lambda: read_battery_extension(dev),
            lambda extension: extension is not None and extension.enabled == enabled and extension.save_status == 0,
            "Battery feedback readback did not confirm the RAM update before timeout.")

        if args.no_save:
            print(f"Battery feedback: {'on' if enabled else 'off'} (RAM only).")
            return

        send_feature_report_checked(dev, bytes((REPORT_SET, FUNC_SAVE)).ljust(FEATURE_REPORT_LEN, b"\x00"),
                                    "Battery feedback")
        poll_readback(
            lambda: read_battery_extension(dev),
            lambda extension: extension is not None and extension.enabled == enabled and extension.save_status == 1,
            "Battery feedback save verification failed before timeout; flash persistence is unconfirmed.")
        print(f"Battery feedback: {'on' if enabled else 'off'} (flash save verified).")
    finally:
        dev.close()


def main():
    parser = argparse.ArgumentParser(description="Read and modify ds5dongle config over USB HID.")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("get", help="read and print the current config").set_defaults(func=cmd_get)
    sub.add_parser("fields", help="list configurable fields and ranges").set_defaults(func=cmd_fields)
    sub.add_parser("reconnect", help="re-enumerate USB so polling interval changes take effect").set_defaults(func=cmd_reconnect)

    p_set = sub.add_parser("set", help="set one or more fields (name=value ...)")
    p_set.add_argument("assignments", nargs="+", metavar="name=value")
    p_set.add_argument("--no-save", action="store_true",
                       help="update RAM only; do not persist to flash")
    p_set.set_defaults(func=cmd_set)

    p_battery = sub.add_parser("battery-feedback", help="read or set the separate battery feedback flag")
    p_battery.add_argument("action", choices=("get", "on", "off"))
    p_battery.add_argument("--no-save", action="store_true",
                           help="update RAM only; do not persist to flash")
    p_battery.set_defaults(func=cmd_battery_feedback)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
