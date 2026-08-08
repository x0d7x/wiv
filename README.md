# wiv — Wayland Input Visualizer

Displays keypresses on screen on supported Wayland compositors (requires
`wlr_layer_shell_v1` support).

https://github.com/user-attachments/assets/285463b6-3560-42e1-b59a-d565bed25464

Forked from [~sircmpwn/wshowkeys](https://git.sr.ht/~sircmpwn/wshowkeys) / [DreamMaoMao/wshowkeys](https://github.com/DreamMaoMao/wshowkeys) — WIV is tailored to my preferences. Credit goes to the original authors.

## Installation

### nix
```bash
nix run github:0xWal/wiv
```

### building from source

Dependencies:

- cairo
- libinput
- pango
- udev
- wayland
- xkbcommon

```bash
$ meson build
$ ninja -C build
```

Build options:
- `-Ddevpath=` — set device path for input capture
- `-Dwsk_debug=true` — enable debug logging (requires debug build)

### permissions

wiv reads `/dev/input/` devices to capture keypresses.
Add your user to the `input` group:

```bash
sudo usermod -aG input $USER
```

Then **log out and back in** for the change to take effect.

After that, run `./build/wiv` directly — no root or setuid required.

## Usage

```
wiv [-b|-f|-s|-r #RRGGBB[AA]] [-F font] [-t timeout]
    [-a top|left|right|bottom|center] [-m margin] [-l lenmax]
    [-o output] [-w [pixels]] [-i] [-H height] [-D trace] [-P] [-R]
    [-O [opacity]] [-c] [-K] [-p[colors]]
    [-g X,Y[,WxH]] [-T left|center|right]
```

- *-b #RRGGBB[AA]*: set background color
- *-f #RRGGBB[AA]*: set foreground color
- *-s #RRGGBB[AA]*: set color for special keys
- *-r #RRGGBB[AA]*: set color for repeat count symbols
- *-F font*: set font (Pango format, e.g. 'monospace bold 24')
- *-t timeout*: set timeout before clearing old keystrokes(ms)
- *-a top|left|right|bottom|center*: anchor the keystrokes to an edge. May be
  specified twice (e.g. `-a top -a left`). `center` can be combined with an
  edge anchor (e.g. `-a top -a center` to center horizontally at the top).
- *-m margin*: set a margin (in pixels) from the nearest edge
- *-M pad*: set the padding string inserted between a held modifier (Ctrl/Alt/Super/Shift)
  and the key it modifies, e.g. `-M ' '` renders `Ctrl C` instead of `CtrlC`.
  Off by default; any `-M` value enables it, `-M ''` explicitly disables.
- *-l lenmax*: set the key layer lenmax
- *-w [pixels]*: use a fixed overlay width instead of resizing per keystroke.
  Without a value, width is computed from `-l` and the current font via Pango.
  With a value, width is `max(pixels, font-computed width for -l)`.
  Text is right-aligned; the overlay hides when empty.
- *-o output*: show only on the specified monitor by name ex: -o eDP-1
- *-i*: inspect mode — show raw xkb keysym names instead of pretty-printed display names
- *-H height*: vertical padding per key row in px (added above/below text, default: 100)
- *-P*: pause — start paused or pause an already-running instance
- *-R*: resume — start normally or resume an already-running instance
- *-D trace*: trace file path for debug logging (debug build only)
- *-O [opacity]*: set global opacity (0.0–1.0). Multiplies the alpha channel of all colors.
  The value must be attached directly to `-O` (no space). When running a second
  instance, communicates with the active instance:
  - `wiv -O.5` — set opacity to 50%
  - `wiv -O` — query current opacity (prints value and exits)
  - `wiv -O"+.1"` — increment opacity by 10%
  - `wiv -O"-.1"` — decrement opacity by 10%
- *-c*: validate keymap config file and exit (prints `OK` or error with line number)
- *-K*: reload keymap config from file (sends reload signal to running instance)
- *-p[colors]*: toggle or set the color pool. Without a value, toggles the pool
  on/off (enabling uses the default pool from `keymap.h`). With a value, enables
  the pool with a comma-separated list of hex colors
  (e.g. `-p"#ff0000,#00ff00,#0000ffAA"`). When used as a second instance, sends
  the toggle/set command to the running instance via IPC. Keys without a per-key
  color use pool colors in display order, wrapping when the pool is exhausted.
- *-g X,Y[,WxH]*: position overlay at absolute coordinates (slurp-compatible).
  Accepts `X,Y` or `X,Y,WxH` (where `x` or `X` separates width from height).
  Spaces are normalized to commas, so slurp output (`X,Y WxH`) works directly.
  When combined with `-a center`, the coordinates become the center point
  instead of the top-left corner.
  When `-g` is used, `-m` (margin) and `-o` (output) are ignored with a warning.
- *-T left|center|right*: set text alignment within a fixed-width overlay (`-w`).
  Independent of the anchor — controls horizontal text position inside the box.
  Default is `left`.

**Environment:**
- *WIV_MASK*: comma-separated key sequence patterns to suppress/sensitive
  input masking

**Pause/Resume:**
wiv is a single-instance application. Running `wiv` a second time fails.
Use `wiv -P` to pause a running instance (clears displayed keys, freezes
the overlay with near-zero CPU usage). Use `wiv -R` to resume it.

**Opacity:**
wiv supports live opacity control via IPC. Launch with `-O<value>` (0.0–1.0)
to set the initial opacity, or adjust a running instance with `wiv -O<value>`.
(The value must be attached directly — no space between `-O` and the number.)

**Color Pool:**
The color pool assigns cycling foreground colors to keys that don't have a
per-key color override. Define the default pool and override behavior in
`keymap.h`:

```c
static const char COLOR_POOL[][10] = {
    "#ff0000",
    "#00ff00",
    "#0000ff",
};
constexpr bool POOL_OVERRIDES_FG = false;
```

- `COLOR_POOL`: array of `#RRGGBB` or `#RRGGBBAA` hex strings. Leave empty to disable.
- `POOL_OVERRIDES_FG`: when `true`, pool colors override per-key colors from
  the keymap config. When `false` (default), per-key colors always win.

Colors are assigned by display position — key #0 gets pool[0], key #1 gets
pool[1], wrapping around. At runtime, `-p"#RRGGBBAA,#RRGGBB"` overrides the
compile-time pool without rebuilding. Run `wiv -p` (no value) on a second
instance to toggle the pool on/off, or `wiv -p"#RRGGBB,#RRGGBB"` to set pool
colors on the running instance via IPC.

**IPC Move:**
A second instance can reposition a running overlay using `-g`. When wiv
detects an already-running instance, it sends the coordinates over IPC and
exits immediately:

```bash
# initial launch
wiv -g 500,800,300x100

# reposition later (slurp output works directly)
wiv -g $(slurp -p)
```

The move command accepts `X,Y` coordinates. When the overlay has a fixed
size (`-g X,Y,WxH`), only the position is sent via IPC.

**Behavior Notes:**
- `-g` overrides both `-m` (margin) and `-o` (output). wiv prints a warning
  when either is combined with `-g`.
- `-a center` and `-g` compose: when both are set, the coordinates specify
  the center of the overlay rather than the top-left corner.
- Use `slurp -p` for point mode — the output (`X,Y`) maps directly to
  `-g` coordinates.

example:
```bash
# basic usage — anchored bottom-right with margin
wiv -a bottom -F 'Sans Bold 30' -s '#B5B520ff' -f  '#ecd29cff' -b '#201B1488' -l 60 -w

# fixed-width with centered text
wiv -w 400 -T center -a bottom -F 'Sans Bold 24'

# position at absolute coordinates via slurp
wiv -g $(slurp -p)

# centered at top with fixed size
wiv -a top -a center -g 960,20,300x60 -T center -F 'Sans Bold 20'
```

## Keymap Configuration

wiv loads a keymap config file on startup to override built-in key display
mappings. The file is loaded only when running as the primary instance.

**Path:** `$XDG_CONFIG_HOME/wiv/keymap` (typically `~/.config/wiv/keymap`).
Falls back to `~/.config/wiv/keymap` if `XDG_CONFIG_HOME` is not set.
If the file doesn't exist, the built-in keymap is used unchanged.

**Format:** one entry per line:

```
<key>|[display]|[color]
```

- `<key>`: xkb keysym name (e.g. `Return`, `space`, `KP_Enter`)
- `[display]`: UTF-8 display text (optional, empty = use key name)
- `[color]`: text color `#RRGGBB` or `#RRGGBBAA` (optional, empty = use global fg)

Escape `|` with `\|` for a literal pipe. Use `\\` for a literal backslash.
Lines starting with `#` are comments. Empty lines are ignored.

**Examples:**

```
# modifier keys
Return|⏎|
space|␣|#ebdbb230
KP_Enter|⏎|

# override just color, keep default display
l||#ff0000

# override just display, keep default color
Escape|⎌|

# both NULL — no-op override
BackSpace||
```

Use `wiv -c` to validate the config file without starting the application.
