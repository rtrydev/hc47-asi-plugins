# HC47 Reduced-Precision x87

A FEX-Emu-style "reduced precision x87" mode for **Hitman: Codename 47**,
delivered as an ASI plugin. Fixes the game's poor performance under
CrossOver/Rosetta 2 on Apple Silicon Macs, where every x87 instruction is
emulated in software (80-bit) while SSE2 runs natively on NEON.

Instead of hand-patching individual hot functions, an offline binary
translator rewrites *every provably-safe x87 function* in the game engine
(`HitmanDlc.dlc`: ~66,000 of ~89,000 x87 instructions across 1,187 functions)
into SSE2 double-precision code. At runtime a small ASI installs `jmp` hooks
at the original function entries.

## How it works

- **`tools/analyze.py`** — function discovery (call targets, prologues,
  vtable/reloc pointer scan) + per-function CFG walk that models the x87
  stack depth at every instruction. A function is *translatable* only if
  everything is statically provable: known depth everywhere, supported ops
  only, no MMX/SSE, no jump tables, no EFLAGS conflicts between integer code
  and inserted SSE compares, balanced stack at calls/rets. Anything else
  stays original ("correctness by rejection").
- **`tools/translate.py`** — generates SSE2 machine code per function:
  x87 stack slot *i* → `xmm_i` (`xmm7` scratch), `fnstsw ax` → `lahf`
  (after `ucomisd`), `call _ftol` → `cvttsd2si`+`cdq`, `fistp` honors a
  shadow control word, rare transcendentals call bit-exact x87 helper stubs.
  Output: a `.x87` patch file (blobs + fixups, validated against the PE
  timestamp).
- **`runtime/`** — `HC47ReducedX87.asi` (32-bit DLL): waits for the module
  to load (ntdll loader notifications), maps the blob, applies fixups,
  installs 5-byte entry hooks.
- **`tests/`** — differential tester run under the CrossOver bottle's wine:
  calls original vs translated leaf functions with randomized contexts and
  compares `eax`/`edx`/`st0`/memory. Current status: 179/179 conclusive
  functions identical.

## Build & install

```sh
brew install mingw-w64
pip3 install capstone pefile

python3 tools/translate.py HitmanDlc.dlc   # writes dist/HitmanDlc.dlc.x87
(cd runtime && make)                        # writes dist/HC47ReducedX87.asi
./install.sh                                # copies into the game's scripts/
./install.sh -u                             # uninstall
```

Requires an ASI loader already working in the game directory (the widescreen
fix setup already provides one). Check `scripts/HC47ReducedX87.log` after
launching — it should say `applied: 1187/1187 hooks`.

## Verify

```sh
python3 tools/gen_manifest.py HitmanDlc.dlc
(cd tests && make)
# copy difftest.exe + dist files into the game dir, then run it with the
# bottle's wine (see tests/run.sh)
```

## Precision notes ("reduced precision")

Translated code computes in 64-bit doubles instead of 80-bit extended
(and instead of the 24-bit single-precision mode Direct3D usually forces).
Same trade-off FEX-Emu's `X87ReducedPrecision` makes; games tolerate it.
If a specific function misbehaves, exclude it by RVA:
`python3 tools/translate.py HitmanDlc.dlc --exclude 0x214f0,0x257c0`.
