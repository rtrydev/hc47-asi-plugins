#!/usr/bin/env python3
"""Translate x87 functions of a game module to SSE2; emit a patch file.

Patch file layout (little-endian):
  char  magic[8]  = "HC47X87P"
  u32   version   = 1
  u32   preferred_base
  u32   timedatestamp        PE header stamp, to reject mismatched binaries
  u32   size_of_image
  u32   n_funcs
  u32   blob_total
  char  module[32]           original module file name, lowercase, NUL-padded
  func table [n_funcs] : u32 rva, blob_off, blob_len, fixup_idx, n_fixups
  u32   n_fixups_total
  fixups [n_total]     : u32 blob_off, u32 arg, u32 type
  blob  [blob_total]
"""
import sys, os, struct, argparse
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hc47x87.module import Module
from hc47x87 import analysis
from hc47x87.codegen import FuncTranslator, TranslateError
from analyze import find_ftol, find_ci, GAME

import capstone

# Functions other mods patch mid-body: hooking them would make those byte
# patches dead code. Known: HitmanCodename47WidescreenFix FOV/draw-distance
# sites (+24e71/+24ec2 in func 24d90, +90167 in func 900c0).
DEFAULT_EXCLUDE = {
    "hitmandlc.dlc": {0x24D90, 0x900C0},
}


def branch_targets(mod):
    """All direct jmp/jcc/call targets module-wide (linear sweep)."""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.skipdata = True
    targets = set()
    for s, e, data in mod.text:
        for ins in md.disasm(data, s):
            m = ins.mnemonic
            if (m == "call" or m == "jmp" or m in analysis.JCC) and \
                    ins.op_str.startswith("0x"):
                try:
                    targets.add(int(ins.op_str, 16))
                except ValueError:
                    pass
    return targets


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("module", nargs="?", default="HitmanDlc.dlc")
    ap.add_argument("--game", default=GAME)
    ap.add_argument("--out", default=None)
    ap.add_argument("--exclude", default="",
                    help="comma-separated function RVAs (hex) to skip")
    ap.add_argument("--diag", action="store_true",
                    help="emit NaN tripwire at float-returning rets")
    args = ap.parse_args()

    path = os.path.join(args.game, args.module)
    mod = Module(path)
    print(f"analyzing {args.module} ...")
    _, funcs0 = analysis.analyze_module(mod)
    ftol = set(find_ftol(mod, funcs0))
    ci = find_ci(mod, funcs0)
    an, funcs = analysis.analyze_full(mod, ftol, ci)

    # never hook the CRT helpers themselves (their callers are rewritten)
    helper_vas = ftol | set(ci)
    excl_rvas = {int(x, 16) for x in args.exclude.split(",") if x}
    excl_rvas |= DEFAULT_EXCLUDE.get(os.path.basename(path).lower(), set())

    # safety: don't hook functions whose first 5 bytes are a branch target
    tgts = branch_targets(mod)
    ok = [f for f in funcs if f.status == "ok" and f.start not in helper_vas
          and (f.start - mod.base) not in excl_rvas]
    entry_hazard = [f for f in ok
                    if any(f.start < t < f.start + 5 for t in tgts)]
    hz = {f.start for f in entry_hazard}
    ok = [f for f in ok if f.start not in hz]
    print(f"translatable: {len(ok)} functions "
          f"({len(entry_hazard)} dropped: entry branch hazard)")

    blob_parts = []
    func_recs = []
    fixup_recs = []
    blob_off = 0
    fails = Counter()
    n_x87_done = 0
    for f in ok:
        try:
            tr = FuncTranslator(mod, f, ftol, ci, diag=args.diag)
            blob, fixups = tr.run()
        except (TranslateError, ValueError) as e:
            fails[str(e)] += 1
            continue
        func_recs.append((f.start - mod.base, blob_off, len(blob),
                          len(fixup_recs), len(fixups)))
        for off, typ, arg in fixups:
            if typ == 6:  # ABS32_BLOB: arg is function-local, make it global
                arg += blob_off
            fixup_recs.append((blob_off + off, arg & 0xFFFFFFFF, typ))
        blob_parts.append(blob)
        blob_off += len(blob)
        n_x87_done += f.n_x87

    blob = b"".join(blob_parts)
    print(f"translated {len(func_recs)} functions, {n_x87_done} x87 insns, "
          f"blob {len(blob)//1024} KB, {len(fixup_recs)} fixups")
    if fails:
        print("codegen failures:")
        for k, n in fails.most_common(10):
            print(f"  {k}: {n}")

    name = os.path.basename(path).lower().encode()[:31]
    out = args.out or os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "dist",
        os.path.basename(path) + ".x87")
    with open(out, "wb") as fh:
        fh.write(b"HC47X87P")
        fh.write(struct.pack(
            "<IIIIII", 1, mod.base,
            mod.pe.FILE_HEADER.TimeDateStamp,
            mod.pe.OPTIONAL_HEADER.SizeOfImage,
            len(func_recs), len(blob)))
        fh.write(name + b"\x00" * (32 - len(name)))
        for rec in func_recs:
            fh.write(struct.pack("<IIIII", *rec))
        fh.write(struct.pack("<I", len(fixup_recs)))
        for rec in fixup_recs:
            fh.write(struct.pack("<III", *rec))
        fh.write(blob)
    print(f"wrote {out} ({os.path.getsize(out)//1024} KB)")


if __name__ == "__main__":
    main()
