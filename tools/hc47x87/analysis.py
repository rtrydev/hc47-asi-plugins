"""Function discovery and per-function x87 translatability analysis.

Strategy: correctness by rejection. A function is only marked OK when the
x87 stack depth is statically known at every instruction, every operation is
in the supported set, and nothing else in the function (MMX/SSE/jump tables/
odd control flow) could interact with our register mapping.
"""
from collections import Counter
from capstone import x86
from . import x87

JCC = {
    "jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja", "js", "jns",
    "jp", "jnp", "jl", "jge", "jle", "jg", "jecxz", "jcxz", "loop",
    "loope", "loopne",
}
RET = {"ret", "retn", "retf"}

X86_OP_REG = x86.X86_OP_REG
X86_OP_MEM = x86.X86_OP_MEM
X86_OP_IMM = x86.X86_OP_IMM

ST_REGS = {getattr(x86, f"X86_REG_ST{i}"): i for i in range(8)}
MM_REGS = {getattr(x86, f"X86_REG_MM{i}") for i in range(8)}
XMM_REGS = {getattr(x86, f"X86_REG_XMM{i}") for i in range(8)}


def st_index(ins):
    """Max ST(i) register index used, or -1."""
    idx = -1
    for op in ins.operands:
        if op.type == X86_OP_REG and op.reg in ST_REGS:
            idx = max(idx, ST_REGS[op.reg])
    return idx


def uses_mmx_or_xmm(ins):
    for op in ins.operands:
        if op.type == X86_OP_REG and (op.reg in MM_REGS or op.reg in XMM_REGS):
            return True
    return ins.mnemonic == "emms"


def mem_size(ins):
    for op in ins.operands:
        if op.type == X86_OP_MEM:
            return op.size
    return 0


class Reject(Exception):
    def __init__(self, reason):
        self.reason = reason


class Site:
    """An x87-relevant instruction with its translation context."""
    __slots__ = ("va", "ins", "depth_in", "kind", "flagsrc")

    def __init__(self, va, ins, depth_in, kind, flagsrc=None):
        self.va = va
        self.ins = ins
        self.depth_in = depth_in
        self.kind = kind          # 'x87' | 'ftol' | 'ci'
        self.flagsrc = flagsrc    # for fnstsw: VA of producing compare


class FuncInfo:
    def __init__(self, start, bound):
        self.start = start
        self.bound = bound          # exclusive upper limit (next function)
        self.end = start            # highest decoded instruction end
        self.status = None          # 'ok' | 'nofp' | reject reason
        self.insns = {}             # va -> capstone insn
        self.depth_in = {}          # va -> x87 depth before insn
        self.order = []             # decoded VAs in ascending order
        self.n_x87 = 0
        self.call_returns = set()   # call VAs treated as pushing st0
        self.max_depth = 0
        self.has_fldcw = False
        self.leaders = set()        # branch-target VAs (block leaders)
        self.flagsrc = {}           # fnstsw VA -> producing compare VA
        self.fnstsw_kind = {}       # fnstsw VA -> 'fresh' | 'stale'
        self.jumptables = {}        # jmp VA -> (table_va, [target VAs])
        self.ret_depths = set()     # x87 depth at each ret


# EFLAGS read/write masks from capstone metadata
_READ_MASK = 0
_WRITE_MASK = 0
for _n in dir(x86):
    if _n.startswith("X86_EFLAGS_TEST_"):
        _READ_MASK |= getattr(x86, _n)
    elif _n.startswith(("X86_EFLAGS_MODIFY_", "X86_EFLAGS_RESET_",
                        "X86_EFLAGS_SET_", "X86_EFLAGS_UNDEFINED_",
                        "X86_EFLAGS_PRIOR_")):
        _WRITE_MASK |= getattr(x86, _n)

FLAG_READERS_EXTRA = {"pushf", "pushfd", "lahf"}

REQ_TWO = {"fpatan", "fyl2x", "fyl2xp1", "fscale",
           "fcompp", "fucompp"}
READS_ST0 = (x87.POP1 | x87.NET0 | {"fptan", "fsincos", "fcompp", "fucompp"}) - {
    "fnop", "wait", "fwait", "fnstsw", "fstsw", "fnstcw", "fstcw", "fldcw",
}


def required_depth(mnem, ins):
    sti = st_index(ins)
    req = 0
    if mnem in REQ_TWO:
        req = 2
    elif mnem in READS_ST0:
        req = 1
    elif mnem == "fld" and sti >= 0:
        req = sti + 1
    if sti >= 0:
        req = max(req, sti + 1)
    return req


class Analyzer:
    def __init__(self, mod, ftol_vas=(), ci_funcs=None, callee_ret=None):
        self.mod = mod
        self.ftol_vas = set(ftol_vas)
        self.ci_funcs = ci_funcs or {}   # va -> ('sin'|'cos'|..., nargs, nret)
        # va -> 0 (returns with empty x87 stack) | 1 (returns value in st0);
        # absent = unknown. Built interprocedurally from a previous pass.
        self.callee_ret = callee_ret or {}
        self.starts = self.discover()

    # ---------------- discovery ----------------
    def discover(self):
        mod = self.mod
        starts = set(mod.exports)
        if mod.in_text(mod.entry):
            starts.add(mod.entry)
        import capstone
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        md.skipdata = True
        for s, e, data in mod.text:
            # linear sweep for call rel32 targets and standard prologues
            for ins in md.disasm(data, s):
                if ins.mnemonic == "call" and ins.op_str.startswith("0x"):
                    try:
                        t = int(ins.op_str, 16)
                        if mod.in_text(t):
                            starts.add(t)
                    except ValueError:
                        pass
            # prologue scan: push ebp; mov ebp, esp
            i = data.find(b"\x55\x8b\xec")
            while i != -1:
                starts.add(s + i)
                i = data.find(b"\x55\x8b\xec", i + 1)
        # vtable / function-pointer scan: relocated 32-bit slots outside .text
        # whose value points into .text. (Switch-case tables live in .text and
        # are deliberately excluded — those targets are mid-function labels.)
        import struct
        for slot in mod.relocs:
            if mod.in_text(slot):
                continue
            raw = mod.read_any(slot, 4)
            if len(raw) == 4:
                v = struct.unpack("<I", raw)[0]
                if mod.in_text(v):
                    starts.add(v)
        return sorted(starts)

    # ---------------- per-function analysis ----------------
    def analyze_function(self, start, bound):
        fi = FuncInfo(start, bound)
        for _ in range(64):  # call_returns fixpoint restarts
            try:
                self._walk(fi)
                break
            except _RetryWalk:
                fi.insns.clear(); fi.depth_in.clear(); fi.order = []
                fi.leaders.clear(); fi.flagsrc.clear()
                fi.fnstsw_kind.clear(); fi.jumptables.clear()
                fi.ret_depths.clear()
                fi.n_x87 = 0; fi.max_depth = 0; fi.end = fi.start
                continue
            except Reject as r:
                fi.status = r.reason
                fi.n_x87 = sum(
                    1 for va in fi.insns
                    if x87.is_x87(fi.insns[va].mnemonic))
                return fi
        else:
            fi.status = "fixpoint"
            return fi
        fi.order = sorted(fi.insns)
        fi.n_x87 = sum(
            1 for va in fi.order
            if x87.is_x87(fi.insns[va].mnemonic)
        )
        if fi.n_x87 == 0 and not any(
            fi.insns[va].mnemonic == "call" and self._call_target(fi.insns[va])
            in (self.ftol_vas | set(self.ci_funcs)) for va in fi.order
        ):
            fi.status = "nofp"
            return fi
        # entry patch needs 5 bytes with no incoming branch into them
        if any(start < l < start + 5 for l in fi.leaders):
            fi.status = "entry-too-small"
            return fi
        if fi.end - fi.start < 5:
            fi.status = "entry-too-small"
            return fi
        fi.status = "ok"
        return fi

    def _call_target(self, ins):
        if ins.mnemonic != "call":
            return None
        op = ins.operands[0]
        if op.type == X86_OP_IMM:
            return op.imm
        return None

    def _walk(self, fi):
        mod = self.mod
        # path state: depth, last_call, last_cmp, flags_ok, owner, ah_saved
        # flags_ok: EFLAGS currently match original-execution EFLAGS
        # owner: VA of most recent EFLAGS writer in translated execution
        # ah_saved: compare VA whose status-AH snapshot is in TEB scratch
        work = [(fi.start, (0, None, None, True, None, None))]
        state = {}
        while work:
            va, st = work.pop()
            prev = state.get(va)
            if prev is not None:
                if prev[0] != st[0]:
                    raise Reject("depth-merge")
                merged = (
                    st[0],
                    prev[1] if prev[1] == st[1] else None,
                    prev[2] if prev[2] == st[2] else None,
                    prev[3] and st[3],
                    prev[4] if prev[4] == st[4] else None,
                    prev[5] if prev[5] == st[5] else None,
                )
                if merged == prev:
                    continue
                state[va] = merged
                st = merged
            else:
                state[va] = st
            depth, last_call, last_cmp, flags_ok, owner, ah_saved = st
            if not (fi.start <= va < fi.bound):
                raise Reject("flow-escape")
            ins = mod.insn_at(va)
            if ins is None:
                raise Reject("bad-decode")
            fi.insns[va] = ins
            fi.depth_in[va] = depth
            fi.end = max(fi.end, va + ins.size)
            mnem = ins.mnemonic
            nxt = va + ins.size

            if uses_mmx_or_xmm(ins):
                raise Reject("mmx-sse")
            if mnem == "int3":
                continue    # trap: copied verbatim, path terminates
            if mnem in x87.UNSUPPORTED:
                raise Reject(f"unsup:{mnem}")

            if x87.is_x87(mnem):
                st2 = self._x87_step(fi, va, ins, st)
                work.append((nxt, st2))
                continue

            # generic EFLAGS accounting for non-x87 instructions
            ef = ins.eflags if hasattr(ins, "eflags") else 0
            reads = bool(ef & _READ_MASK) or mnem in FLAG_READERS_EXTRA
            writes = bool(ef & _WRITE_MASK)
            if reads and not flags_ok:
                raise Reject("flags-clobber")

            if mnem == "call":
                tgt = self._call_target(ins)
                if tgt in self.ftol_vas:
                    if depth < 1:
                        raise Reject("ftol-underflow")
                    depth -= 1
                    last_call = None
                elif tgt in self.ci_funcs:
                    _, nargs, nret = self.ci_funcs[tgt]
                    if depth < nargs:
                        raise Reject("ci-underflow")
                    depth += nret - nargs
                    last_call = None
                else:
                    # depth > 0 is fine: live slots are carried across the
                    # call on the real x87 stack (codegen spill/reload)
                    kr = self.callee_ret.get(tgt) if tgt is not None else None
                    if kr == 1:
                        # callee provably returns a value in st0
                        fi.call_returns.add(va)
                        depth += 1
                        if depth > 7:
                            raise Reject("depth8")
                        last_call = None
                    elif kr == 0:
                        # callee provably returns with an empty x87 stack:
                        # it can never be marked as an st0 source
                        fi.call_returns.discard(va)
                        last_call = None
                    else:
                        if va in fi.call_returns:
                            depth += 1
                            if depth > 7:
                                raise Reject("depth8")
                        last_call = va
                work.append((nxt, (depth, last_call, None, True, va, None)))
                continue

            if writes:
                flags_ok, owner = True, va

            if mnem in RET:
                if depth not in (0, 1):
                    raise Reject("ret-depth")
                fi.ret_depths.add(depth)
                continue

            st2 = (depth, last_call, last_cmp, flags_ok, owner, ah_saved)

            if mnem in ("jmp", "ljmp"):
                op = ins.operands[0]
                if op.type != X86_OP_IMM:
                    targets = self._jump_table(fi, va, ins)
                    for t in targets:
                        fi.leaders.add(t)
                        if fi.start <= t < fi.bound:
                            work.append((t, st2))
                        elif depth != 0:
                            raise Reject("tailjmp-depth")
                    continue
                t = op.imm
                if fi.start <= t < fi.bound:
                    fi.leaders.add(t)
                    work.append((t, st2))
                else:
                    if depth != 0:
                        raise Reject("tailjmp-depth")
                    if not mod.in_text(t):
                        raise Reject("flow-escape")
                continue

            if mnem in JCC:
                if mnem in ("jecxz", "jcxz", "loop", "loope", "loopne"):
                    raise Reject("loop-insn")
                op = ins.operands[0]
                if op.type != X86_OP_IMM:
                    raise Reject("indirect-jcc")
                t = op.imm
                if fi.start <= t < fi.bound:
                    fi.leaders.add(t)
                    work.append((t, st2))
                else:
                    if depth != 0:
                        raise Reject("tailjmp-depth")
                    if not mod.in_text(t):
                        raise Reject("flow-escape")
                work.append((nxt, st2))
                continue

            work.append((nxt, st2))

    def _jump_table(self, fi, va, ins):
        """MSVC switch: jmp [table + reg*4], optionally through a byte table.
        Returns list of targets; records fi.jumptables[va]. Raises Reject if
        the pattern can't be proven."""
        mod = self.mod
        op = ins.operands[0]
        if op.type != X86_OP_MEM:
            raise Reject("indirect-jmp")
        m = op.mem
        if m.base != 0 or m.index == 0 or m.scale != 4:
            raise Reject("indirect-jmp")
        table_va = m.disp & 0xFFFFFFFF
        if not mod.in_text(table_va):
            raise Reject("jt-table-loc")

        # scan back for the bounding `cmp <reg>, imm` and optional byte-table
        # movzx between it and the jmp. The cmp must be on the index register
        # (or the byte-table index) or the bound is meaningless.
        import struct
        bound = None
        byte_table = None
        scan = []
        back = va
        cmp_reg = None
        for _ in range(8):
            prevs = [p for p in fi.insns if p < back and
                     p + fi.insns[p].size == back]
            if len(prevs) != 1:
                break
            back = prevs[0]
            scan.append(fi.insns[back])
            if fi.insns[back].mnemonic == "cmp":
                ops = fi.insns[back].operands
                if len(ops) == 2 and ops[0].type == X86_OP_REG and \
                        ops[1].type == X86_OP_IMM:
                    bound = ops[1].imm
                    cmp_reg = ops[0].reg
                break
        if bound is None or not (0 <= bound < 4096):
            raise Reject("jt-nobound")
        index_regs = {m.index}
        for s in scan:
            if s.mnemonic == "movzx" and len(s.operands) == 2 and \
                    s.operands[1].type == X86_OP_MEM and \
                    s.operands[1].size == 1:
                byte_table = s.operands[1].mem.disp & 0xFFFFFFFF
                index_regs.add(s.operands[1].mem.index)
        # match on register name modulo width (cmp al/ax/eax variants)
        rn = self.mod.md.reg_name
        names = {rn(r).lstrip("e").replace("l", "x") for r in index_regs if r}
        cn = rn(cmp_reg).lstrip("e").replace("l", "x") if cmp_reg else None
        if cn is None or cn not in names:
            raise Reject("jt-cmpreg")

        if byte_table is not None:
            raw = mod.read(byte_table, bound + 1)
            if len(raw) != bound + 1:
                raise Reject("jt-bytetable")
            n_entries = max(raw) + 1
        else:
            n_entries = bound + 1

        raw = mod.read(table_va, n_entries * 4)
        if len(raw) != n_entries * 4:
            raise Reject("jt-read")
        targets = list(struct.unpack(f"<{n_entries}I", raw))
        for t in targets:
            if not (fi.start <= t < fi.bound):
                raise Reject("jt-target-out")
        fi.jumptables[va] = (table_va, targets)
        return targets

    def _x87_step(self, fi, va, ins, st):
        depth, last_call, last_cmp, flags_ok, owner, ah_saved = st
        mnem = ins.mnemonic
        if mnem in ("wait", "fwait", "fnop"):
            return st
        if mnem in x87.RESET:
            return (0, last_call, None, flags_ok, owner, ah_saved)

        if mnem in ("fnstsw", "fstsw"):
            if ins.op_str not in ("ax", "eax"):
                raise Reject("fnstsw-mem")
            if last_cmp is None:
                raise Reject("fnstsw-nosrc")
            # translated compares snapshot EFLAGS to TEB; fnstsw reads the
            # snapshot without touching EFLAGS, so intervening flag writers
            # are harmless.
            fi.flagsrc[va] = last_cmp
            self._check_c1(fi, va)
            return (depth, last_call, last_cmp, flags_ok, owner, ah_saved)

        if mnem in ("fnstcw", "fstcw", "fldcw"):
            fi.has_fldcw = True
            return st

        # 80-bit / BCD memory forms
        msz = mem_size(ins)
        if msz == 10:
            raise Reject("fp80")

        req = required_depth(mnem, ins)
        if depth < req:
            # maybe the value comes from a call that returns in st0
            if last_call is not None and last_call not in fi.call_returns:
                fi.call_returns.add(last_call)
                raise _RetryWalk()
            raise Reject("underflow")
        depth += x87.depth_effect(mnem)
        if depth > 7:
            # depth 8 would need xmm7 (our scratch)
            raise Reject("depth8" if depth == 8 else "overflow")
        fi.max_depth = max(fi.max_depth, depth)
        if mnem in x87.COMPARES:
            # translated to ucomisd: writes EFLAGS (diverges from original)
            last_cmp, flags_ok, owner = va, False, va
        elif mnem in ("fist", "fistp"):
            # translated form tests the shadow control word: writes EFLAGS
            flags_ok, owner = False, va
        return (depth, last_call, last_cmp, flags_ok, owner, ah_saved)

    def _check_c1(self, fi, va):
        """Reject if code after fnstsw ax inspects C1 (AH bit 1) or AL."""
        mod = self.mod
        cur = va + fi.insns[va].size if va in fi.insns else va
        ins = mod.insn_at(va)
        cur = va + ins.size
        for _ in range(4):
            nxt = mod.insn_at(cur)
            if nxt is None:
                return
            m, ops = nxt.mnemonic, nxt.op_str
            if m in ("test", "and", "cmp") and ops.startswith("ah,"):
                try:
                    imm = int(ops.split(",")[1], 0)
                    if imm & 0x02:
                        raise Reject("c1-used")
                except ValueError:
                    raise Reject("c1-used")
                return
            if m == "sahf":
                return
            if m in ("test", "and", "cmp", "or") and (
                    ops.startswith("al,") or ops.startswith("eax,") or ops.startswith("ax,")):
                raise Reject("al-used")
            cur += nxt.size


class _RetryWalk(Exception):
    pass


def analyze_module(mod, ftol_vas=(), ci_funcs=None, callee_ret=None):
    an = Analyzer(mod, ftol_vas, ci_funcs, callee_ret)
    funcs = []
    starts = [s for s in an.starts if mod.in_text(s)]
    an.starts = starts
    for i, s in enumerate(starts):
        bound = starts[i + 1] if i + 1 < len(starts) else None
        if bound is None:
            for ts, te, _ in mod.text:
                if ts <= s < te:
                    bound = te
        funcs.append(an.analyze_function(s, bound))
    return an, funcs


def ret_depth_map(funcs):
    """va -> 0|1 for functions whose walk completed with a uniform ret depth."""
    out = {}
    for f in funcs:
        if f.status in ("ok", "nofp") and len(f.ret_depths) == 1:
            out[f.start] = next(iter(f.ret_depths))
    return out


def analyze_full(mod, ftol_vas, ci_funcs, iterations=3):
    """Iterate analysis so interprocedural st0-return knowledge reaches a
    fixpoint: each pass unlocks functions whose ret depth informs the next."""
    callee_ret = {}
    an = funcs = None
    for _ in range(iterations):
        an, funcs = analyze_module(mod, ftol_vas, ci_funcs, callee_ret)
        new_map = ret_depth_map(funcs)
        if new_map == callee_ret:
            break
        callee_ret = new_map
    return an, funcs


def coverage_report(mod, funcs, total_x87):
    ok_x87 = sum(f.n_x87 for f in funcs if f.status == "ok")
    analyzed_x87 = sum(f.n_x87 for f in funcs)
    rejects = Counter()
    for f in funcs:
        if f.status not in ("ok", "nofp"):
            rejects[f.status] += f.n_x87
    lines = [
        f"functions: {len(funcs)}  ok: {sum(1 for f in funcs if f.status=='ok')}"
        f"  nofp: {sum(1 for f in funcs if f.status=='nofp')}",
        f"x87 insns in module (census): {total_x87}",
        f"x87 insns in analyzed funcs:  {analyzed_x87}",
        f"x87 insns in OK funcs:        {ok_x87}"
        f"  ({100.0*ok_x87/max(analyzed_x87,1):.1f}% of analyzed)",
        "top reject reasons (weighted by x87 insns):",
    ]
    for r, n in rejects.most_common(15):
        lines.append(f"  {r:24} {n}")
    return "\n".join(lines)
