# Touch controls design: a data-driven on-screen gamepad and keyboard

Date: 2026-09-17
Status: proposed
Parent spec: `2026-09-13-recomp-kit-design.md`; supersedes the fixed tables and
the "no per-game key list" decision of `2026-09-13-keypad-design.md`

## 1. Goal

Give every game the kit builds two on-screen control sets a player can
reshape: a PlayStation-style gamepad and a keyboard. Both are layouts of one
widget system, stored as JSON, shipped per game and editable on the device.
The gamepad also drives games through a real DirectInput joystick when the
game reads one, and through keys and mouse when it does not. A physical
controller feeds the same path as the on-screen one.

## 2. Decisions taken in conversation

- One widget system for pad and keyboard (approach A). Not a second overlay
  stack beside the keypad, not a scripting layer.
- Pad output is chosen per game: `native` (the game sees a DirectInput
  joystick) or `mapped` (pad input becomes keys and mouse through a table in
  `game.toml`).
- Customization is layout files plus an in-game editor reached from F10:
  move, resize, add, remove and rebind controls; save to the profile; reset
  to the game default.
- Build order is incremental, each step shippable (section 11).
- Work happens on branch `touch-controls` off `main`, in its own worktree.

## 3. Non-goals

- XInput and winmm `joy*`. No current port imports them; they are added
  when a game does, on the same virtual pad (section 7).
- Text entry. The four-finger system keyboard toggle stays.
- Themes, skins and images in layouts. One look: the hud pipeline and the
  6x8 font, with the PS face glyphs drawn as shapes.
- Rumble / force feedback. DirectInput effects stay unsupported.
- More than one virtual pad (local multiplayer).
- Portrait layouts. Layouts are landscape; iPhone gets its own default file
  if a game wants one, but no separate design.

## 4. Concepts

- **Control**: one on-screen element. Kinds:
  - `key`: holds a scancode while touched. Modifier scancodes use the
    existing Off/Held/Latched/Locked machine (`keypad_modifiers`).
  - `button`: a pad button (`cross`, `circle`, `square`, `triangle`, `l1`,
    `r1`, `l2`, `r2`, `l3`, `r3`, `select`, `start`, `ps`). `l2`/`r2` also
    set their trigger axis to full while held.
  - `dpad`: four directions plus diagonals from the touch angle (45 degree
    sectors, 25% centre dead zone). Sliding between sectors changes the held
    direction.
  - `stick`: `left` or `right`. `fixed` stays where it is drawn; `floating`
    recentres on the first touch inside its region. Output is the offset
    from centre over the radius, clamped to the unit circle, with a radial
    dead zone (default 0.15) rescaled so output starts at 0.
  - `toggle`: shows or hides a named group, or switches to a named layout.
  - `action`: a host action (`settings`, `system_keyboard`, `edit_layout`).
- **Group**: a named set of controls that toggles together (the two keypad
  halves are groups `left` and `right`).
- **Layout**: a named list of groups and controls, e.g. `pad`, `keys`,
  `pad+keys`.
- **Virtual pad (vpad)**: 13 buttons, a 4-bit hat, and six axes (LX, LY, RX,
  RY, L2, R2) in [-1, 1] / [0, 1]. Pad controls and physical controllers
  write it; the binding stage reads it.
- **Binding stage**: turns the vpad into what the game sees (section 7).

## 5. Layout files

JSON, one layout per file. Geometry is in points, anchored to a screen edge or
corner inside the safe area, so a layout fits any screen size without
per-device files.

```json
{
  "version": 1,
  "name": "pad",
  "opacity": 0.6,
  "groups": [
    { "id": "sticks", "visible": true, "controls": [
      { "kind": "stick", "stick": "left", "mode": "floating",
        "anchor": "bottom-left", "x": 40, "y": 40, "radius": 70 },
      { "kind": "button", "button": "cross",
        "anchor": "bottom-right", "x": 90, "y": 40, "size": 56 },
      { "kind": "key", "scancode": "F5", "label": "SAVE",
        "anchor": "top-right", "x": 20, "y": 20, "w": 64, "h": 36 }
    ]}
  ]
}
```

- `anchor` is one of the four corners, the four edge midpoints or `center`;
  `x`/`y` are the inset from that anchor to the control's matching corner.
- `size` (square/round controls) or `w`/`h`; `radius` for sticks.
- `scancode` is a name from the existing `KeypadScan` table ("A", "F5",
  "LShift"). `label` defaults to the key name.
- Unknown fields are ignored and unknown `kind`s are skipped with a log
  line, so newer files load in older kits. A file that fails to parse is
  ignored and the game default is used; the error is logged once and shown
  on the F10 page.

Where files come from, first match wins:

1. `<profile>/controls/<name>.json`: the player's edited copy.
2. `<game data>/controls/<name>.json`: shipped by the game repo
   (`layouts/` in the repo, bundled by `tools/build.py`).
3. Built into the kit: `keys` (today's split keypad, byte-for-byte the
   same tables, now as embedded JSON) and `pad` (a generic DualSense
   layout).

The store keeps only `host.controls/layout` (the active layout's index in
the discovered list) and `host.controls/opacity`. The existing
`host.keypad/left`, `/right` and `/size` values migrate on first run: left
and right become the group visibility of the `keys` layout, and size becomes
a scale factor (0.89, 1.0, 1.11) on that layout. The `host.keypad/*` keys are
then no longer written.

## 6. Touch routing

`ControlsRouter` replaces the keypad branch of the finger handler in
`host/sdl/main.cpp`:

- Finger down: hit-test the visible controls, topmost first. A hit claims
  the finger for that control until lift or cancel; a miss goes to
  `TouchMapper` as today. The space between controls in a group's bounding
  box is claimed and does nothing, as the keypad's gaps do now.
- Finger motion goes to the owning control (sticks and dpad use it; keys
  and buttons ignore it, so sliding off still holds).
- Several fingers can own several controls at once (stick + button + key).
- A hardware keyboard hides layouts that contain only `key` controls. A
  connected physical controller hides pad-only layouts. Layouts that mix
  the two stay visible, and the player can override this on F10.

## 7. Output

### 7.1 Keys and host actions

`key` controls emit the same key down/up the keypad does now, through
`handle_key`, so DirectInput, `WM_KEYDOWN`/`WM_CHAR` and
`GetAsyncKeyState` all see them. `action` controls call the existing host
entry points.

### 7.2 The vpad

`host/controls/vpad.h`: a lock-free snapshot written on the input thread and
read on the game thread when it polls, plus an edge queue for buffered
reads. Sources:

- On-screen `button`/`dpad`/`stick` controls.
- Physical controllers through `SDL_Gamepad` (the launcher already opens
  them; the game path now opens the first one too). SDL's mapping names
  the DualSense buttons, so no per-controller table is needed.

When both are active their values are merged: buttons are ORed and, for
each axis, the value with the larger magnitude wins.

### 7.3 `native`: DirectInput joystick

`dx/dinput.cpp` gains a joystick device kind:

- `EnumDevices` lists one `DI8DEVTYPE_GAMEPAD` instance ("Recomp Virtual
  Pad") when `[controls] pad = "native"`, for the joystick filters
  (`DI8DEVCLASS_GAMECTRL`, `DIEDFL_ATTACHEDONLY`).
- `CreateDevice` accepts that instance GUID (and `GUID_Joystick`) instead of
  returning `DIERR_DEVICENOTREG`.
- `SetDataFormat` accepts `DIJOYSTATE` and `DIJOYSTATE2` and any custom format
  whose objects it can place. `EnumObjects` reports X, Y, Z, Rx, Ry, Rz, one
  POV and 13 buttons. `GetObjectInfo`, `GetProperty` and `SetProperty` handle
  `DIPROP_RANGE`, `DIPROP_DEADZONE`, `DIPROP_SATURATION` and
  `DIPROP_BUFFERSIZE`.
- `GetDeviceState` scales the vpad to the set range. `GetDeviceData`
  drains the edge queue. `Poll` returns `DI_OK`. `Acquire`/`Unacquire`
  behave as they do for the mouse.
- Axis order follows the common DualShock-on-DirectInput layout: LX→X,
  LY→Y, RX→Z, RY→Rz, L2→Rx, R2→Ry; buttons square, cross, circle, triangle,
  L1, R1, L2, R2, select, start, L3, R3, PS. A game that expects another
  order gets a `[controls.native]` remap table in `game.toml`.

With `pad = "mapped"` or `"off"`, `CreateDevice` behaves as it does today.

### 7.4 `mapped`: keys and mouse

`host/controls/binding.h` evaluates a table from `game.toml` each input tick:

```toml
[controls]
default_layout = "pad"      # "pad" | "keys" | "pad+keys" | "hidden"
pad = "mapped"              # "native" | "mapped" | "off"

[controls.mapped]
left_stick = "cursor"       # "cursor" | "arrows" | "wasd" | "scroll" | "none"
right_stick = "scroll"
cursor_speed = 900          # points per second at full deflection
cross = "mouse_left"
circle = "mouse_right"
square = "key:Space"
triangle = "key:Tab"
l1 = "key:PageUp"
r1 = "key:PageDown"
l2 = "mouse_middle"
r2 = "key:LShift"
start = "key:Escape"
select = "key:F10"
dpad = "arrows"
```

- Targets: `key:<name>`, `mouse_left|right|middle`, `wheel_up|down`,
  `action:<host action>`, `none`.
- `cursor` moves the host cursor with speed curved by deflection squared,
  through the same synthetic mouse path touch uses, so the absolute
  cursor, relative DirectInput motion and `WM_MOUSEMOVE` stay consistent.
- `arrows`/`wasd` press the keys when an axis passes 0.5 and release them
  below 0.35 (hysteresis).
- `scroll` emits the same pan action `TouchMapper` produces for a
  two-finger drag (`kTouchPanStep` per tick at full deflection).
- Players override single entries in the F10 editor; overrides are saved
  to `<profile>/controls/binding.json` and read on top of the `game.toml`
  table.
- `tools/gen_game_config.py` turns `[controls]` into the generated config
  header, replacing `RECOMP_TOUCH_KEYPAD_DEFAULT`. `[touch] keypad` is still
  read as a deprecated spelling of `default_layout` ("auto" → "keys",
  "hidden" → "hidden") until every game repo is re-pinned.

## 8. Drawing

`host/controls/overlay.{h,cpp}` replaces `keypad_overlay`: it rasterizes each
visible control to a CPU canvas at drawable size, uploads it once per
layout, size or lit-state change, and blends it in the presenter at the
point the keypad uses today (`present_thread.cpp`). `host_present_set_keypad`
becomes `host_present_set_controls(ControlsView)`.

- Keys: as today (rounded rectangle, 6x8 font at 2x, lit when latched or
  locked).
- Buttons: circles. ✕ ○ □ △ are drawn as strokes in the four PlayStation
  colours; shoulders are pill shapes with text labels.
- Stick: base ring plus a knob at the current offset. It is the only
  control that redraws every frame while held, so the knob is a separate
  small quad and the canvas is not re-uploaded.
- Pressed state: brighter fill. The layout's `opacity` scales everything.

## 9. Editor

Reached from the F10 page ("Edit controls") or an `edit_layout` action.

- The game is paused through the pause the settings page already uses.
  The frame is dimmed and a 10 pt grid is drawn.
- Drag moves a control, and its anchor changes to the nearest corner or edge
  on drop. A pinch resizes it. Controls snap to the grid and to each other's
  edges.
- A toolbar at the top offers Add (a palette of key, button, dpad, stick
  and toggle), Delete (the selected control), Bind, Layout (switch,
  duplicate, rename), Reset (restore the game default) and Done.
- Bind opens a picker. For a key control it shows the keypad's key list.
  For pad buttons in mapped mode it shows the target list from 7.4. In
  native mode it shows the pad button list.
- Done writes `<profile>/controls/<name>.json` atomically (temp file,
  then rename) and leaves the editor. Reset deletes that file.
- The editor uses the router and overlay. Its own hit test runs first
  while it is open, so controls do not fire while being edited.
- On desktop the mouse drives the editor the same way; the scroll wheel
  resizes.

## 10. Components

| File | Responsibility |
|---|---|
| `host/controls/layout.{h,cpp}` | Model, JSON load/save, source lookup, anchor → drawable-pixel geometry, hit test. SDL-free. |
| `host/controls/builtin_layouts.cpp` | Embedded `keys` and `pad` JSON. |
| `host/controls/router.{h,cpp}` | Finger ownership, control state (held, stick offset, dpad sector), modifier machine use; emits key, vpad and action events. SDL-free. |
| `host/controls/vpad.{h,cpp}` | Snapshot, edge queue, source merge. |
| `host/controls/binding.{h,cpp}` | Mapped-mode table and evaluation; emits key/mouse actions. SDL-free. |
| `host/controls/gamepad_sdl.cpp` | `SDL_Gamepad` open/close/hotplug → vpad. |
| `host/controls/overlay.{h,cpp}` | Rasterize and blend; replaces `keypad_overlay`. |
| `host/controls/editor.{h,cpp}` | Edit mode state, toolbar, picker, save/reset. |
| `host/keypad_layout.*`, `keypad_modifiers.*` | Scan names and modifier machine kept; half tables move into `builtin_layouts.cpp`. |
| `mods/controls_settings.{h,cpp}` | Replaces `keypad_settings`: layout, opacity, visibility overrides, migration. |
| `dx/dinput.cpp` | Joystick device (7.3). |
| `host/sdl/main.cpp`, `host/present*.{h,cpp}` | Wiring. |
| `tools/gen_game_config.py`, `games/stub/game.toml` | `[controls]`. |
| `tools/build.py` | Bundle a game repo's `layouts/`. |
| `host/tests/controls_*_tests.cpp`, `dx/tests/dinput_joystick_tests.cpp` | Section 12. |

## 11. Build order

1. **Layout model + keypad port.** The JSON model, the router, and the
   overlay drawing `key` controls; the `keys` builtin; the settings
   migration. There is no visible change: the iPad keypad looks and behaves
   exactly as before.
2. **Pad + mapped binding.** Button, dpad and stick controls; the vpad; the
   binding stage; the `pad` builtin; `[controls]` in `game.toml`; the F10
   layout switch. First target: Majesty on the iPad.
3. **Physical controllers.** `SDL_Gamepad` → vpad, and auto-hiding.
4. **Native DirectInput joystick.** First target: NFSMW (handed to the
   NFSMW session to pin).
5. **Editor.** Also covers binding overrides.

Each step is its own PR to `main` with CI green on all platforms before
the next starts.

## 12. Testing

- Unit tests, all SDL-free:
  - Layout parse, round-trip and error handling, with unknown fields and
    kinds.
  - Anchor geometry at several drawable sizes and safe areas.
  - Hit order.
  - Stick dead zone and clamping.
  - Dpad sectors.
  - Multi-finger ownership.
  - Merge rules.
  - Binding hysteresis and cursor speed.
  - Settings migration from `host.keypad/*`.
- A step-1 regression test: the builtin `keys` layout yields the same key
  rectangles and scancodes as the old tables, at all three sizes.
- DirectInput tests through the fake backend: enumeration, `CreateDevice`,
  both data formats, range and dead-zone properties, buffered data.
- On the device, for each step:
  - iPad keypad unchanged (step 1).
  - Majesty playable with the pad alone (step 2).
  - A DualSense over Bluetooth on the iPad and on the Mac (step 3).
  - NFSMW steering and throttle through the analog stick and triggers
    (step 4).
  - A layout edited, saved, relaunched and reset on the iPad (step 5).

## 13. Risks

- **Game UI hidden under controls.** The default layouts leave the centre
  free, and opacity is adjustable.
- **Axis-order expectations** differ between games. `[controls.native]`
  remap covers this without code.
- **Editor scope.** It is last in the order, so steps 1 to 4 are useful
  without it. Players can still edit JSON files in the meantime.
