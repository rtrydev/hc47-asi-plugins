# HC47 ASI Plugins

ASI plugins and supporting tools for **Hitman: Codename 47** on modern
systems, especially CrossOver/Rosetta 2 on Apple Silicon Macs.

The repo currently builds five independent runtime plugins:

- `HC47ReducedX87.asi` — translates safe x87-heavy engine functions to SSE2
  double-precision code at runtime to improve CrossOver/Rosetta performance.
- `HC47HudScale.asi` — scales the game's 2D GUI/HUD layer natively in any
  window mode, with optional sharper TrueType text.
- `HC47HudExtras.asi` — adds HUD quality-of-life features: smaller crosshair
  dot, mission timer, and FPS readout.
- `HC47Widescreen.asi` — native widescreen support: arbitrary hitman.ini
  resolutions and aspect-correct FOV.
- `HC47FrameLimit.asi` — configurable FPS cap (the engine has none of its
  own, and high frame rates cause gameplay and camera bugs).

The plugins can be used together or individually. They are byte-checked
against the supported retail game build; a mismatch disables the affected
patch path rather than blindly patching unknown code.

## Build & Install

### mac

```sh
brew install mingw-w64
pip3 install capstone pefile

python3 tools/translate.py HitmanDlc.dlc   # writes dist/HitmanDlc.dlc.x87
(cd runtime && make)                        # writes the ASI plugins into dist/
./install.sh                                # copies plugins into scripts/
./install.sh -u                             # uninstall generated plugin files
```

### Windows

The same sources build natively with MSYS2's 32-bit mingw-w64 gcc; the
Makefile picks the right compiler by platform, and the generated `.x87`
patch files are byte-identical to the mac-built ones.

```powershell
winget install MSYS2.MSYS2
C:\msys64\usr\bin\pacman.exe -S --noconfirm mingw-w64-i686-gcc make
pip install capstone pefile

.\build.ps1 -Translate -Install    # build + regenerate .x87 + install
```

`build.ps1` wraps the same `make` / `translate.py` / `install.sh` steps
with the MSYS2 toolchain on PATH; `install.sh` also runs fine from Git
Bash directly.

The build includes its own ASI loader (`dist/dsound.dll`, installed into
the game root), so no third-party loader is needed — see the ASI Loader
section below.

By default `install.sh` targets the platform's default Steam install:

```text
mac:     /Users/rtry/Library/Application Support/CrossOver/Bottles/Steam/drive_c/Program Files (x86)/Steam/steamapps/common/Hitman Codename 47
Windows: C:\Program Files (x86)\Steam\steamapps\common\Hitman Codename 47
```

Override it with `HC47_GAME_DIR=/path/to/game ./install.sh` (the same
variable is honored by `tools/analyze.py` / `tools/translate.py`).

## Renderer and window-mode support

The renderer is chosen by the `DrawDll` line in `Hitman.ini`; what works
depends on the platform:

| | RenderD3D | RenderOpenGL |
|---|---|---|
| mac / CrossOver | any resolution; fullscreen becomes a borderless window at desktop resolution | untested |
| Windows | max 2048px per axis (auto-clamped) | any resolution, fullscreen + windowed |

On modern Windows the legacy Direct3D 7 HAL (emulated by `ddraw.dll`)
refuses render targets larger than 2048px per axis, in windowed and
fullscreen mode alike — the stock game dies with "Unable to initialize
Direct3D". The Widescreen plugin detects this (real Windows only, never
under Wine) and clamps the requested resolution to the best fitting
display mode (fullscreen) or window size (windowed) so the game always
starts; for native 4K on Windows switch `DrawDll` to `RenderOpenGL.dll`.

Fullscreen additionally requires the exact `hitman.ini` resolution to
exist in the driver's display-mode list; a miss makes the stock game die
with "Unable to find a suitable display mode for true color. Try
changing to 16bit colors." (common under CrossOver, whose mode list
rarely contains the exact requested resolution). The Widescreen plugin
validates the resolution against the mode list before the renderer
starts and falls back to the closest available mode.

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

The first apply is timing-critical: the renderer mirrors the requested
resolution into the current-mode fields already at window creation,
hundreds of ms before display-mode selection reads the requested fields
back. Writing the virtual size in that window makes fullscreen mode
selection pick a real display mode of the *virtual* size, silently
downgrading a 4K request to 1080p with an unscaled GUI. The plugin
therefore applies from an ntdll loader notification when `HitmanDlc.dlc`
loads — after renderer init is fully done, before any GUI layout code
(which lives in that DLL) can run — and gates its re-apply watchdog on
the same condition.

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
`runtime/rtrace.c` and can be built with `make rtrace` from `runtime/`;
`runtime/wmdiag.c` (`make wmdiag`) plants one-shot breakpoint probes on the
RenderD3D display-init path and logs which step fails with what HRESULT.

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

## Plugin: Widescreen (`HC47Widescreen.asi`)

This plugin makes the game render correctly at any resolution and aspect
ratio. It needs no resolution configuration of its own: it takes the
resolution from hitman.ini and reads the live display mode from
`ZSysInterface` at run time, so it composes cleanly with `HC47HudScale`.
Three patch groups:

- **Resolution passthrough** — the renderer's mode-set path snaps the
  requested width to a fixed 4:3 ladder (512..1600) in exclusive
  fullscreen. The plugin flips the guarding `je` to an unconditional jump
  in whichever renderer is loaded (`RenderD3D.dll` / `RenderOpenGL.dll`),
  so any `Resolution WxH` from hitman.ini passes through unmodified.
  (The stock ladder is also broken for widths it does not know: it loads
  an uninitialized height and the game dies with the "16bit colors"
  mode error, so the passthrough fixes stock fullscreen at non-ladder
  resolutions too.)
- **Borderless fullscreen** (default under Wine/CrossOver) — exclusive
  DirectDraw fullscreen is broken under the CrossOver Mac driver: the
  emulated mode change renders oversized on scaled Retina desktops, the
  exclusive-mode cursor clipping freezes the mouse, and alt-tab loses the
  exclusive surfaces (permanent black window). When hitman.ini requests
  fullscreen and `Borderless` is on (`-1` auto-enables it under Wine
  only), the request is converted before the renderer loads: the
  fullscreen flag is cleared and the resolution is replaced with the
  current desktop size. The game's own windowed path already creates an
  undecorated popup window, so this alone yields borderless fullscreen;
  the window is only moved if its client area is not at 0,0 — its style
  is never touched, because restyling a live DirectDraw device window
  blacks out rendering under the CrossOver Mac driver. Real Windows keeps
  exclusive fullscreen by default.

  The in-game options screen stays usable with this: the desktop
  resolution is added to its resolution list (the list, its labels and
  the applied values all come from a static mode table in the renderer,
  which is replaced with an extended copy through its accessor), and the
  same fullscreen-to-borderless conversion is installed as a hook on the
  renderer's mode-set path, so toggling fullscreen in the menu converts
  on the fly instead of re-entering broken exclusive fullscreen. The
  screen also opens with the mode actually running selected (stock
  restores the selection from the saved profile — a hand-edited
  hitman.ini shows up as the 800x600 default); the selection hook feeds
  the matcher the live mode from ZSysInterface and writes it into the
  options state, so applying without changes keeps the current mode.
- **Resolution guard** — before the renderer runs mode selection, the
  requested resolution is validated: in fullscreen it must exist in the
  `EnumDisplaySettings` mode list, and with `RenderD3D` on real Windows
  both axes must fit the emulated D3D7 HAL's 2048px render-target limit.
  A failing request is clamped to the best working resolution (see
  "Renderer and window-mode support" above) instead of letting the game
  die with a fatal display-mode error.
- **FOV correction** — the real display mode is read from `ZSysInterface`
  (the current-mode fields `HC47HudScale` leaves real) and every FOV
  source gets the standard Vert- to Hor+ correction
  `new = 2*atan(tan(old/2) * (aspect / (4/3)))`. The six camera setups
  pushing the 67.4-degree constant into `SetFOV` are rewritten in place
  (re-applied if the mode changes at run time); the cutscene and sniper
  scope FOVs come from script data, so those sites get entry hooks that
  correct the loaded value per call. `FOVFactor` applies on top of the
  gameplay camera only.
- **Draw distance** (optional) — the far-clip value handed to the renderer
  at camera activation is scaled by `DrawDistanceFactor` when it is not
  1.0.

All patch sites are byte-checked against the retail build (module timestamp
and image size included); a mismatch skips that patch path and logs it.

Config: `scripts/HC47Widescreen.ini`

```ini
[Widescreen]
Enabled=1
FOVFactor=1.0          ; extra factor on the gameplay camera FOV
DrawDistanceFactor=1.0 ; 1.0 leaves the draw distance untouched
Borderless=-1          ; fullscreen -> borderless window at desktop
                       ; resolution: -1 auto (on under Wine, off on
                       ; real Windows), 0 never, 1 always
```

Install output:

- ASI: `scripts/HC47Widescreen.asi`
- Config: `scripts/HC47Widescreen.ini`
- Log: `scripts/HC47Widescreen.log`

No other widescreen or FOV patcher should be active at the same time —
two patchers on the same sites either double-patch or fail their byte
checks.

## ASI Loader (`dsound.dll`)

A minimal dsound.dll proxy (`runtime/asiloader.c`) that loads every
`*.asi` from `scripts/` at process attach and logs to
`scripts/HC47AsiLoader.log`.

The game pulls it in through its sound stack: `EAX.dll` imports dsound
ordinal 1, `Sound.dll` ordinal 2, and `a3d.dll` imports by name. The
proxy therefore exports all 12 documented dsound.dll entry points at
their real ordinals (`runtime/dsound.def`) and forwards them to the
system dsound.dll, resolved lazily on first call. The ordinal layout is
load-bearing: a proxy with a different export order silently misbinds the
by-ordinal imports (generic universal loaders get this wrong for this
game).

Under Wine/CrossOver the bottle needs the DLL override
`dsound=native,builtin` so the game-directory proxy is picked over the
builtin — any bottle where an ASI loader ever worked already has it
(check with `grep dsound user.reg` in the bottle directory).

## Plugin: Frame Limit (`HC47FrameLimit.asi`)

The engine has no frame limiter: the per-frame game-time latch in
System.dll copies current to previous time and samples a new timestamp
with no pacing anywhere in the main loop. On fast machines the frame rate
is whatever vsync (or nothing) allows, and running much above 60 fps is
known to cause gameplay and camera bugs in this engine.

The plugin hooks the latch's clock call site (byte-checked; the latch
entry itself belongs to `HC47HudExtras`'s frame tick) and stalls each
frame until the configured period has elapsed, measured with
`QueryPerformanceCounter` — sleeping while more than a few milliseconds
remain, spinning the rest for accuracy. Pacing happens before the engine
samples its clock, so the measured frame delta stays clean at the cap.
The deadline advances one period per frame and resyncs after loads or
hitches instead of fast-forwarding.

Config: `scripts/HC47FrameLimit.ini`

```ini
[FrameLimit]
FpsCap=60        ; frames per second; 0 disables the cap
```

Install output:

- ASI: `scripts/HC47FrameLimit.asi`
- Config: `scripts/HC47FrameLimit.ini`
- Log: `scripts/HC47FrameLimit.log`

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
