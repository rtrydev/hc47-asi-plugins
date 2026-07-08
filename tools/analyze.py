#!/usr/bin/env python3
"""Analyze game modules: function discovery, x87 classification, coverage."""
import sys, os, argparse
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hc47x87.module import Module
from hc47x87 import analysis, x87

# Default game location: HC47_GAME_DIR env, else the platform's default
# Steam install (CrossOver bottle on mac, Program Files on Windows).
GAME = os.environ.get("HC47_GAME_DIR") or (
    "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Hitman Codename 47"
    if os.name == "nt" else
    os.path.expanduser(
        "~/Library/Application Support/CrossOver/Bottles/Steam/"
        "drive_c/Program Files (x86)/Steam/steamapps/common/Hitman Codename 47"))


def census(mod):
    """Total x87 instruction count + mnemonic histogram via linear sweep."""
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.skipdata = True
    total = 0
    hist = Counter()
    for s, e, data in mod.text:
        for ins in md.disasm(data, s):
            if 0xD8 <= ins.bytes[0] <= 0xDF:
                total += 1
                hist[ins.mnemonic] += 1
    return total, hist


def find_ftol(mod, an_funcs):
    """MSVC _ftol: small function with fnstcw + fldcw + fistp m64, no calls."""
    out = []
    for f in an_funcs:
        if not f.insns or f.end - f.start > 0x60:
            continue
        mnems = [f.insns[v].mnemonic for v in sorted(f.insns)]
        if "call" in mnems:
            continue
        has64 = any(
            f.insns[v].mnemonic == "fistp" and analysis.mem_size(f.insns[v]) == 8
            for v in f.insns)
        if has64 and ("fnstcw" in mnems or "fstcw" in mnems) and "fldcw" in mnems:
            out.append(f.start)
    return out


CI_PATTERNS = [
    # (name, marker mnemonics that must appear, nargs, nret, max size)
    ("sin",   {"fsin"},   1, 1, 0x60),
    ("cos",   {"fcos"},   1, 1, 0x60),
    ("sqrt",  {"fsqrt"},  1, 1, 0x40),
    ("atan2", {"fpatan"}, 2, 1, 0x60),
    ("fmod",  {"fprem"},  2, 1, 0x80),
    ("log",   {"fyl2x"},  1, 1, 0x60),
    ("tan",   {"fptan"},  1, 1, 0x80),
]


def find_ci(mod, an_funcs):
    """MSVC _CIxxx CRT helpers: take arg(s) in st0(,st1), return in st0.

    They read st0 at entry without loading it, so pass-1 analysis rejects
    them with 'underflow'. Confirm by linear decode: small, leaf, contains
    the marker transcendental.
    """
    out = {}
    for f in an_funcs:
        if f.status != "underflow":
            continue
        size = f.bound - f.start
        if size > 0x100:
            continue
        mnems = set()
        va = f.start
        while va < f.bound:
            ins = mod.insn_at(va)
            if ins is None:
                break
            mnems.add(ins.mnemonic)
            if ins.mnemonic in ("ret", "retn", "int3"):
                break
            va += ins.size
        if "call" in mnems:
            continue
        for name, markers, na, nr, maxsz in CI_PATTERNS:
            if markers <= mnems:
                out[f.start] = (name, na, nr)
                break
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("module", nargs="?", default="HitmanDlc.dlc")
    ap.add_argument("--game", default=GAME)
    args = ap.parse_args()

    path = os.path.join(args.game, args.module)
    print(f"=== {args.module} ===")
    mod = Module(path)
    total_x87, hist = census(mod)

    # pass 1: no helper knowledge, to locate CRT helpers
    an, funcs = analysis.analyze_module(mod)
    ftol = find_ftol(mod, funcs)
    ci = find_ci(mod, funcs)
    print(f"_ftol candidates: {[hex(v) for v in ftol]}")
    print(f"_CI candidates:   {{{', '.join(f'{hex(v)}: {n[0]}' for v, n in ci.items())}}}")

    # pass 2: with helper knowledge
    an, funcs = analysis.analyze_full(mod, ftol, ci)
    print(analysis.coverage_report(mod, funcs, total_x87))
    print("\nx87 mnemonic census (top 25):")
    for m, n in hist.most_common(25):
        print(f"  {m:12} {n}")


if __name__ == "__main__":
    main()
