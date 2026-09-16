# Keyboard hold mappings

Requires a firmware build with `ENABLE_WAKE_HID` and the existing
`enable_keyboard=1` (or `enable_wake=1`) setting. Reconnect USB after enabling
the keyboard interface. The normal build without `ENABLE_WAKE_HID` retains
the configuration but does not emit keyboard reports.

Using `tools/config_tool.py`, assign existing shortcut slots with `@hold`:

```powershell
python tools/config_tool.py set enable_keyboard=1
# Unplug/replug the adapter here to enumerate the keyboard interface.
python tools/config_tool.py shortcut "1=Cross@hold:Space" "2=L1@hold:Ctrl+C"
python tools/config_tool.py shortcut "3=L1+R1@hold:Shift"
```

Hold mappings press immediately and release when the physical button releases.
For a two-button trigger, both must be down; releasing either releases the
output. These mappings are independent of single/double-tap arbitration.
Existing shortcut syntax still produces a timed pulse, not a hold.

Each slot supports modifiers plus one ordinary keyboard key, including
modifier-only mappings. Active slots are merged, deduplicating shared keys
and modifiers. The existing report supports six distinct ordinary keys;
more than six emits HID ErrorRollOver until the count returns to six or less.
A timed shortcut shares the same keyboard state; a key already held stays held
rather than being released/repressed to force another shortcut event.

Controller input is preserved by default. To suppress a mapped source in the
gamepad report, use the existing remap command, e.g. `remap Cross=disable`.
Keyboard triggers read the physical state before gamepad remapping.

## Wire format / review notes

- Existing report `0xFB`, nine 7-byte slots, unchanged storage layout.
- New flags bit `0x02` (`SHORTCUT_FLAG_HOLD`). Only keyboard actions accept it.
- `trigger_b=0xFD` plus this flag means a single-button hold; a real button ID
  means a two-button hold. Combining hold with double-tap is invalid.
- Configuration replacement, Bluetooth disconnect and USB mount reset state.
  All-up reports retry from the main loop when the endpoint becomes ready,
  even without another Bluetooth input packet. Pending timed presses also retry
  from this service loop. A busy consumer endpoint does not block keyboard release.
- The wake sequence temporarily owns the shared keyboard endpoint; hold/pulse
  output pauses during its F15 sequence and resumes with fresh physical input.
- Unchanged keyboard state is cached: the main loop skips rebuilding reports
  until a hold, pulse or reset changes it.
- Old firmware does not understand the new flag and normalizes these slots
  to disabled; existing slots keep their meanings on the new firmware.
- The companion ds5dongle-config-web editor supports hold and hold-chord modes,
  including controller capture and the same flag validation.
- Keyboard support still uses `ENABLE_WAKE_HID`; decoupling that build option
  remains outside this implementation.

Run host regression checks with `python tests/test_hold.py` (Python and `g++`).
Hardware validation is still needed for USB suspend/resume, disconnects and
application-specific shortcut/autorepeat behavior.
