#!/usr/bin/env python3
"""Emit a test manifest: RVAs of translated LEAF functions (no calls, no
indirect control flow) — safe to probe-call in the differential tester."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hc47x87.module import Module
from hc47x87 import analysis
from analyze import find_ftol, find_ci, GAME


def main():
    module = sys.argv[1] if len(sys.argv) > 1 else "HitmanDlc.dlc"
    mod = Module(os.path.join(GAME, module))
    _, funcs0 = analysis.analyze_module(mod)
    ftol = set(find_ftol(mod, funcs0))
    ci = find_ci(mod, funcs0)
    _, funcs = analysis.analyze_full(mod, ftol, ci)
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                       "dist", module + ".leaf.txt")
    n = 0
    with open(out, "w") as fh:
        for f in funcs:
            if f.status != "ok" or f.start in ftol or f.start in ci:
                continue
            mnems = [f.insns[v].mnemonic for v in f.order]
            if "call" in mnems:
                continue
            fh.write(f"{f.start - mod.base:x}\n")
            n += 1
    print(f"wrote {n} leaf functions to {out}")


if __name__ == "__main__":
    main()
