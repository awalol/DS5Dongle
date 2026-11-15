# Keyboard hold mappings

The base firmware includes `ENABLE_WAKE_HID`. Enable the existing
`enable_keyboard=1` (or `enable_wake=1`) setting. Reconnect USB after enabling
the keyboard interface. A custom build without `ENABLE_WAKE_HID` retains
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

## Keep modifiers held

Use `@modifier_hold` for a two-button mapping whose first button (A) keeps the
modifiers held and whose second button (B) controls the regular key:

```powershell
python tools/config_tool.py shortcut "1=L1+R1@modifier_hold:Alt+Tab"
python tools/config_tool.py shortcut "2=Create+Options@modifier_hold:Ctrl+Shift+Tab"
```

Either button alone produces no output from this mapping. Holding both buttons,
in either order, activates it. Keep A down and release B: only the regular key
releases. Press B again to press that key again while the modifiers stay down.
Releasing A ends the mapping, even if B is still held. Holding B keeps the key
down; key repeat is handled by the host, as with a physical keyboard.

The order in the configuration assigns roles: `L1+R1` and `R1+L1` are different.
At least one modifier and one regular key are required. Single-button triggers,
double taps, media actions and firmware actions are not supported by this mode.
In the web editor, select **Hold chord to press** (组合键跟随) and turn on
**Keep modifiers held** (保持修饰键). The editor labels the A/B roles next to their
selectors. Controller capture requires two buttons; an
incomplete capture leaves the configuration unchanged.

This mode claims single/double-tap mappings on A and B for the active interaction,
including repeated B presses while A remains down. Separate `@hold` mappings
remain independent. Gamepad output is preserved as with other keyboard mappings.

## Wire format / review notes

- Existing report `0xFB`, nine 7-byte slots, unchanged storage layout.
- Flags bit `0x02` (`SHORTCUT_FLAG_HOLD`). Only keyboard actions accept it.
- Flags bit `0x04` (`SHORTCUT_FLAG_MODIFIER_HOLD`) selects modifier hold. It is
  mutually exclusive with hold and double-tap flags and requires two physical
  buttons, nonzero modifiers and a regular key in `0x04..SHORTCUT_KEY_USAGE_MAX`.
- `trigger_b=0xFD` plus this flag means a single-button hold; a real button ID
  means a two-button hold. Combining hold with double-tap is invalid.
- Configuration replacement, Bluetooth disconnect, USB suspend and mount reset state.
  All-up reports retry from the main loop when the endpoint becomes ready,
  even without another Bluetooth input packet. Pending timed presses also retry
  from this service loop. A busy consumer endpoint does not block keyboard release.
- The wake sequence temporarily owns the shared keyboard endpoint; hold/pulse
  output pauses during its F15 sequence and resumes with fresh physical input.
- The config UI's "listen to controller" capture sends `0xF6` funcid `0x05`
  with `buffer[1] = 1`. The firmware ignores controller input and pauses
  shortcut output for `SHORTCUT_LEARNING_WINDOW_MS` (5 s), releasing anything
  already held when the window opens. The window cannot be cancelled; the UI
  stops listening on the same five-second schedule.
- Unchanged keyboard state is cached: the main loop skips rebuilding reports
  until a hold, pulse or reset changes it.
- Old firmware does not understand the new flag and normalizes these slots
  to disabled; existing slots keep their meanings on the new firmware.
- The companion ds5dongle-config-web editor supports hold, hold-chord and modifier-hold modes,
  including controller capture and the same flag validation.
- Keyboard support still uses `ENABLE_WAKE_HID`; decoupling that build option
  remains outside this implementation.

Run host regression checks with `python tests/test_hold.py` (Python and `g++`).
Hardware validation is still needed for USB suspend/resume, disconnects and
application-specific shortcut/autorepeat behavior.
