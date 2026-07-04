# HC47 ASI Plugins

ASI plugins and supporting tools for **Hitman: Codename 47** on modern
systems, especially CrossOver/Rosetta 2 on Apple Silicon Macs.

The repo currently builds three independent runtime plugins:

- `HC47ReducedX87.asi` — translates safe x87-heavy engine functions to SSE2
  double-precision code at runtime to improve CrossOver/Rosetta performance.
- `HC47HudScale.asi` — scales the game's 2D GUI/HUD layer natively in any
  window mode, with optional sharper TrueType text.
- `HC47HudExtras.asi` — adds HUD quality-of-life features: smaller crosshair
  dot, mission timer, and FPS readout.

The plugins can be used together or individually. They are byte-checked
against the supported retail game build; a mismatch disables the affected
patch path rather than blindly patching unknown code.

## Build & Install

```sh
brew install mingw-w64
pip3 install capstone pefile

python3 tools/translate.py HitmanDlc.dlc   # writes dist/HitmanDlc.dlc.x87
(cd runtime && make)                        # writes the ASI plugins into dist/
./install.sh                                # copies plugins into scripts/
./install.sh -u                             # uninstall generated plugin files
```

Requires an ASI loader already working in the game directory. The widescreen
fix setup already provides one.

By default `install.sh` targets:

```text
/Users/rtry/Library/Application Support/CrossOver/Bottles/Steam/drive_c/Program Files (x86)/Steam/steamapps/common/Hitman Codename 47
```

Override it with `HC47_GAME_DIR=/path/to/game ./install.sh`.

## Plugin: Reduced x87 (`HC47ReducedX87.asi`)

This plugin improves performance under CrossOver/Rosetta 2, where x87
instructions are emulated in software while SSE2 runs much closer to native
speed. It uses an offline translator to rewrite provably-safe x87 functions
into SSE2 double-precision code, then a small ASI loader installs 5-byte
entry hooks when the matching game module loads.

Current `HitmanDlc.dlc` coverage is about 73,000 of 89,000 x87 instructions
across 1,382 functions. Anything not statically proven safe stays original:
unsupported ops, unknown x87 stack depth, jump tables, MMX/SSE conflicts,
EFLAGS hazards, and unbalanced call/return state are rejected.

Key pieces:

- `tools/analyze.py` discovers functions and builds per-function control-flow
  state, including x87 stack depth.
- `tools/translate.py` emits `.x87` patch blobs plus fixups, validated against
  PE timestamp and image size.
- `runtime/hc47x87.c` loads `.x87` files from `scripts/HC47ReducedX87/`,
  allocates translated code, applies fixups, and hooks original functions.
- `tests/` contains the differential tester used to compare original and
  translated leaf functions under Wine/CrossOver.

Install output:

- ASI: `scripts/HC47ReducedX87.asi`
- Patch blobs: `scripts/HC47ReducedX87/*.x87`
- Log: `scripts/HC47ReducedX87.log`

Expected log after launch includes the number of applied hooks, for example
`applied: 1187/1187 hooks`.

### Diagnostic: EIP-sampling profiler (`HC47Profile.asi`)

Not part of the default build. A sampling profiler for deciding where the
remaining CrossOver/Rosetta time goes — untranslated x87 functions, other
modules, or the graphics stack:

```sh
(cd runtime && make prof)
cp dist/HC47Profile.asi "$HC47_GAME_DIR/scripts/"
```

It suspends each running game thread ~250 times per second, samples EIP via
`GetThreadContext`, and appends a report to `scripts/HC47Profile.log` every
10 s: per-module sample shares, the hottest 16-byte code buckets as
`module+rva`, and — by following the installed entry hooks and the `.x87`
func tables — per-function counts for translated code, keyed by original
RVA. Because translated entries are hooked out of the module, samples inside
game-module `.text` are untranslated code: the top-bucket list is the
priority list for extending translation coverage. Fully idle threads are
filtered out so Wine service threads don't flood the histogram. Remove the
ASI when done; sampling costs a few percent of frame time.

### Precision Notes

Translated code computes in 64-bit doubles instead of 80-bit extended
precision. This is the same broad trade-off as FEX-Emu's
`X87ReducedPrecision`: games generally tolerate it, and the translator avoids
functions where correctness is not mechanically clear.

If a specific translated function misbehaves, exclude it by RVA:

```sh
python3 tools/translate.py HitmanDlc.dlc --exclude 0x214f0,0x257c0
```

## Plugin: HUD Scale (`HC47HudScale.asi`)

This plugin scales the game's 2D layer: menus, HUD, text, and cursor. It works
in exclusive fullscreen, borderless, and windowed modes.

The game lays out GUI elements against the resolution fields in
`ZSysInterface`, while the renderer maps normalized GUI coordinates to its
real display mode. `HC47HudScale.asi` waits for display setup, then keeps
`width / Scale` and `height / Scale` pinned into the GUI-facing
`ZSysInterface` fields. The GUI lays out for a smaller virtual screen and the
renderer stretches it back to the real viewport. The 3D scene and actual
display mode are not changed.

`SharpText=1` additionally patches the TrueType font path so text is
rasterized at the real pixel size instead of being magnified from the smaller
virtual layout.

Config: `scripts/HC47HudScale.ini`

```ini
[HudScale]
Scale=2.0        ; 1.0 disables; fractional values such as 1.5 work
SharpText=1      ; rasterize TTF fonts at real pixel size
```

Install output:

- ASI: `scripts/HC47HudScale.asi`
- Config: `scripts/HC47HudScale.ini`
- Log: `scripts/HC47HudScale.log`

Diagnostic tooling used while reverse-engineering the GUI pipeline lives in
`runtime/rtrace.c` and can be built with `make rtrace` from `runtime/`.

## Plugin: HUD Extras (`HC47HudExtras.asi`)

This plugin adds small HUD features without changing the 3D renderer.

Features:

- **Crosshair scale** shrinks the aiming dot. The active retail HUD dot is a
  `ZGEOMREF`; the plugin scales only its private generated vertex buffer and
  never mutates the shared source transform, avoiding world-geometry side
  effects. Older or alternate crosshair objects exposed as `ZWINOBJ` still use
  the engine's native `SetScale`.
- **Mission timer** renders elapsed mission time as HUD text below the
  top-left health/armor display. It uses the game's own `BankGothic10` font
  and parents labels to the in-mission display group.
- **FPS readout** renders a second HUD label below the timer, derived from the
  engine's own frame timestamps.

Config: `scripts/HC47HudExtras.ini`

```ini
[HudExtras]
CrosshairScale=0.5   ; 1.0 leaves the crosshair untouched
ShowTimer=1
ShowFPS=1
TextX=10             ; overlay position in virtual GUI pixels
TextY=100            ; just below the top-left base-game HUD
LineGap=16
```

Install output:

- ASI: `scripts/HC47HudExtras.asi`
- Config: `scripts/HC47HudExtras.ini`
- Log: `scripts/HC47HudExtras.log`

## Verification

For the x87 translator:

```sh
python3 tools/gen_manifest.py HitmanDlc.dlc
(cd tests && make)
```

Then copy `difftest.exe` and the relevant `dist` files into the game
directory and run it with the bottle's Wine environment. See `tests/run.sh`
for the local runner shape.

For runtime plugins, launch the game and inspect the corresponding log in the
game's `scripts/` directory. Each plugin writes its own log file and reports
which hooks or patch paths were installed.
