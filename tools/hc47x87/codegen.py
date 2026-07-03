"""x87 -> SSE2 function translation.

Maps the statically-known x87 stack to xmm0..xmm6 (slot i = xmm_i, bottom of
stack = slot 0), xmm7 is scratch. Non-x87 instructions are copied verbatim
(with relocation fixups); control flow is re-encoded with rel32 forms.

ESP adjustments inside inserted sequences use `lea esp,[esp+d]`, which does
not touch EFLAGS, so copied flag-dependent code keeps working.

Fixup types (resolved by the runtime loader):
  0 ABS32_MODULE  *p += load_delta                      (arg unused)
  1 REL32_MODULE  *p  = arg + load_delta - (site+4)     (arg = target VA)
  2 REL32_HELPER  *p  = helper[arg] - (site+4)
  3 ABS32_DATA    *p  = data_base + arg
"""
import struct
from capstone import x86
from . import x87 as xt
from .analysis import JCC, RET, X86_OP_IMM, X86_OP_REG, X86_OP_MEM, ST_REGS
from . import encoder as E
from .encoder import Mem

ABS32_MODULE, REL32_MODULE, REL32_HELPER, ABS32_DATA, \
    TEB_FP, TEB_AH, ABS32_BLOB = range(7)

# helper ids (must match runtime)
H_SIN, H_COS, H_SINCOS, H_TAN, H_ATAN2, H_YL2X, H_YL2XP1, H_2XM1, \
    H_SCALE, H_RNDINT, H_I64TOD, H_DTOI64, H_DIAGNAN, H_EMPTYPOP = range(14)

# data area offsets (must match runtime)
D_ZERO, D_ONE, D_PI, D_L2E, D_L2T, D_LG2, D_LN2 = 0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30
D_SIGNMASK, D_ABSMASK = 0x40, 0x50
D_SHADOW_CW = 0x60          # u32; RC bits live in byte D_SHADOW_CW+1, bits 2-3

FLDCONST = {"fld1": D_ONE, "fldz": D_ZERO, "fldpi": D_PI, "fldl2e": D_L2E,
            "fldl2t": D_L2T, "fldlg2": D_LG2, "fldln2": D_LN2}

CC_OPC = {  # jcc -> 0F xx rel32 opcode
    "jo": 0x80, "jno": 0x81, "jb": 0x82, "jae": 0x83, "je": 0x84,
    "jne": 0x85, "jbe": 0x86, "ja": 0x87, "js": 0x88, "jns": 0x89,
    "jp": 0x8A, "jnp": 0x8B, "jl": 0x8C, "jge": 0x8D, "jle": 0x8E,
    "jg": 0x8F,
}


class TranslateError(Exception):
    pass


class Item:
    """One emitted chunk: raw bytes with fixups, or a branch placeholder."""
    __slots__ = ("data", "fixups", "branch", "jt")

    def __init__(self, data=b"", fixups=None, branch=None, jt=None):
        self.data = data
        self.fixups = fixups or []   # (off, type, arg)
        self.branch = branch         # (kind, cc_byte, target_va) kind: jmp/jcc/call
        self.jt = jt                 # jump-table jmp: original jmp VA


class FuncTranslator:
    def __init__(self, mod, fi, ftol_vas, ci_funcs, diag=False, pc24=False):
        self.mod = mod
        self.fi = fi
        self.ftol_vas = ftol_vas
        self.ci_funcs = ci_funcs
        self.diag = diag      # emit NaN tripwire at float returns
        # pc24: emulate x87 precision-control = single by rounding every
        # arithmetic result to float. NOT needed for this game (measured
        # in-game CW is PC=double, which SSE doubles already match); kept
        # for ports to games that do run at PC=single.
        self.pc24 = pc24
        self.items = []       # (va_or_None, Item)
        self.va_index = {}    # original va -> item list index (for labels)

    # ------------- emission primitives -------------
    def emit(self, data, fixups=None):
        self.items.append(Item(bytes(data), fixups))

    def emit_branch(self, kind, cc, target):
        self.items.append(Item(branch=(kind, cc, target)))

    def emit_mem(self, pair):
        """pair = (bytes, disp32_off) from encoder; attaches module fixup if
        the ORIGINAL instruction had a relocation (set by caller)."""
        data, doff = pair
        fix = []
        if self._cur_reloc and doff is not None:
            fix.append((doff, ABS32_MODULE, 0))
        self.emit(data, fix)

    def emit_data_mem(self, prefix_opc, reg, data_off):
        """SSE op with absolute [data_area+off] operand."""
        m = Mem(disp=0)
        data, doff = E.sse_rm(prefix_opc[0], prefix_opc[1], reg, m)
        self.emit(data, [(doff, ABS32_DATA, data_off)])

    # ------------- TEB scratch (fs:) accessors -------------
    def teb_movsd_store(self, x):
        """movsd fs:[TEB_FP], xmm_x"""
        self.emit(bytes([0x64, 0xF2, 0x0F, 0x11, 0x05 | (x << 3), 0, 0, 0, 0]),
                  [(5, TEB_FP, 0)])

    def teb_movsd_load(self, x):
        self.emit(bytes([0x64, 0xF2, 0x0F, 0x10, 0x05 | (x << 3), 0, 0, 0, 0]),
                  [(5, TEB_FP, 0)])

    def teb_fld(self):
        self.emit(b"\x64\xDD\x05\x00\x00\x00\x00", [(3, TEB_FP, 0)])

    def teb_fstp(self):
        self.emit(b"\x64\xDD\x1D\x00\x00\x00\x00", [(3, TEB_FP, 0)])

    def spill_before_call(self, d):
        """Carry xmm slots 0..d-1 across a call on the real x87 stack,
        exactly reproducing the original FPU stack the callee saw."""
        for i in range(d):
            self.teb_movsd_store(i)
            self.teb_fld()

    def reload_after_call(self, d, returns_st0):
        if returns_st0:
            self.teb_fstp()
            self.teb_movsd_load(d)
        for i in range(d - 1, -1, -1):
            self.teb_fstp()
            self.teb_movsd_load(i)

    # ------------- helpers -------------
    def _free_gp(self, mem):
        for r in (E.EAX, E.ECX, E.EDX):
            if mem is None or not mem.uses(r):
                return r
        raise TranslateError("no free gp")

    def _mem_op(self, ins):
        for op in ins.operands:
            if op.type == X86_OP_MEM:
                return op, Mem.from_op(op)
        return None, None

    def _st_ops(self, ins):
        return [ST_REGS[op.reg] for op in ins.operands
                if op.type == X86_OP_REG and op.reg in ST_REGS]

    def diag_check_slot(self, slot):
        """--diag: record if xmm slot just became NaN (H_DIAGNAN checks)."""
        if not self.diag:
            return
        self.emit(E.lea_esp(-8))
        self.emit(E.movsd_espmem_xmm(0, slot))
        self.emit(b"\xE8\x00\x00\x00\x00", [(1, REL32_HELPER, H_DIAGNAN)])
        self.emit(E.lea_esp(8))

    def post_arith(self, slot, is_div=False):
        """Applied after every arithmetic result: 24-bit rounding (PC=single
        emulation) and optional NaN tripwire."""
        if self.pc24:
            self.emit(E.sse_rr(*E.CVTSD2SS, 7, slot))
            self.emit(E.sse_rr(*E.CVTSS2SD, slot, 7))
        if is_div:
            self.diag_check_slot(slot)

    def helper1(self, hid, x):
        """st0-in/st0-out helper on xmm slot x."""
        self.emit(E.lea_esp(-8))
        self.emit(E.movsd_espmem_xmm(0, x))
        self.emit(b"\xE8\x00\x00\x00\x00", [(1, REL32_HELPER, hid)])
        self.emit(E.movsd_xmm_espmem(x, 0))
        self.emit(E.lea_esp(8))

    def helper2(self, hid, a_slot, b_slot, out_slot, out_from=0):
        """two-in helper: [esp]=a, [esp+8]=b; result read from [esp+out_from]."""
        self.emit(E.lea_esp(-16))
        self.emit(E.movsd_espmem_xmm(0, a_slot))
        self.emit(E.movsd_espmem_xmm(8, b_slot))
        self.emit(b"\xE8\x00\x00\x00\x00", [(1, REL32_HELPER, hid)])
        self.emit(E.movsd_xmm_espmem(out_slot, out_from))
        self.emit(E.lea_esp(16))

    # ------------- main -------------
    def run(self):
        fi = self.fi
        for va in fi.order:
            self.va_index[va] = len(self.items)
            self._cur_reloc = bool(self.mod.relocs_in(va, fi.insns[va].size))
            self.translate_insn(va, fi.insns[va])
        return self.layout()

    def translate_insn(self, va, ins):
        fi = self.fi
        mnem = ins.mnemonic
        d = fi.depth_in[va]
        t = d - 1

        if xt.is_x87(mnem):
            self.x87_insn(va, ins, mnem, d, t)
            return

        if mnem == "call":
            op = ins.operands[0]
            tgt = op.imm if op.type == X86_OP_IMM else None
            if tgt in self.ftol_vas:
                # _ftol(st0) -> edx:eax, truncating
                self.emit(E.sse_rr(*E.CVTTSD2SI, E.EAX, t))
                self.emit(E.CDQ)
                return
            if tgt in self.ci_funcs:
                self.ci_call(self.ci_funcs[tgt][0], d, t)
                return
            returns_st0 = va in fi.call_returns
            if d > 0:
                self.spill_before_call(d)
            if tgt is not None:
                self.emit_branch("call", None, tgt)
            else:
                self.copy(va, ins)  # indirect call: copy verbatim
            if d > 0:
                self.reload_after_call(d, returns_st0)
            elif returns_st0:
                if va in fi.verified_returns:
                    # callee provably returned a value in st0
                    self.emit(E.lea_esp(-8))
                    self.emit(E.FSTP_QWORD_ESP)
                    self.emit(E.movsd_xmm_espmem(0, 0))
                    self.emit(E.lea_esp(8))
                else:
                    # unverified (indirect/opaque callee): the dynamic target
                    # may not have pushed st0. fxam-check; pop if present,
                    # else record (diag) and substitute 0.0.
                    self.emit(
                        b"\x50"                      # push eax
                        b"\x9C"                      # pushfd
                        b"\xD9\xE5"                  # fxam
                        b"\xDF\xE0"                  # fnstsw ax
                        b"\x80\xE4\x45"              # and ah, 0x45
                        b"\x80\xFC\x41"              # cmp ah, 0x41 (empty)
                        b"\x74\x14"                  # je Lempty
                        b"\x9D"                      # popfd
                        b"\x58"                      # pop eax
                        b"\x8D\x64\x24\xF8"          # lea esp,[esp-8]
                        b"\xDD\x1C\x24"              # fstp qword [esp]
                        b"\xF2\x0F\x10\x04\x24"      # movsd xmm0,[esp]
                        b"\x8D\x64\x24\x08"          # lea esp,[esp+8]
                        b"\xEB\x0F"                  # jmp Ldone
                        b"\x9D"                      # Lempty: popfd
                        b"\x58"                      # pop eax
                        b"\xE8\x00\x00\x00\x00"      # call H_EMPTYPOP
                        b"\xF2\x0F\x10\x05\x00\x00\x00\x00",  # movsd xmm0,[zero]
                        [(37, REL32_HELPER, H_EMPTYPOP),
                         (45, ABS32_DATA, D_ZERO)])
            return

        if mnem in RET:
            if d == 1:
                # ABI: leave return value in real st0
                self.emit(E.lea_esp(-8))
                self.emit(E.movsd_espmem_xmm(0, 0))
                if self.diag:
                    # NaN tripwire: helper checks [esp+4], records caller
                    self.emit(b"\xE8\x00\x00\x00\x00",
                              [(1, REL32_HELPER, H_DIAGNAN)])
                self.emit(E.FLD_QWORD_ESP)
                self.emit(E.lea_esp(8))
            self.copy(va, ins)
            return

        if mnem in ("jmp", "ljmp"):
            if va in fi.jumptables:
                # rebuilt pointer table lives at the end of the blob; the
                # disp32 (and each entry) gets an ABS32_BLOB fixup whose arg
                # is assigned during layout
                mem = Mem.from_op(ins.operands[0])
                mem.disp = 0
                enc, doff = mem.encode(4)   # FF /4
                self.items.append(Item(b"\xFF" + enc,
                                       [(1 + doff, ABS32_BLOB, -1)], jt=va))
                return
            self.emit_branch("jmp", None, ins.operands[0].imm)
            return

        if mnem in JCC:
            self.emit_branch("jcc", CC_OPC[mnem], ins.operands[0].imm)
            return

        self.copy(va, ins)

    def copy(self, va, ins):
        fix = []
        for rva in self.mod.relocs_in(va, ins.size):
            fix.append((rva - va, ABS32_MODULE, 0))
        self.emit(bytes(ins.bytes), fix)

    # ------------- x87 translation -------------
    def x87_insn(self, va, ins, mnem, d, t):
        E_ = E
        mop, mem = self._mem_op(ins)
        msz = mop.size if mop is not None else 0
        sts = self._st_ops(ins)

        def X(slot):
            if not 0 <= slot <= 6:
                raise TranslateError(f"bad slot {slot}")
            return slot

        if mnem in ("wait", "fwait", "fnop"):
            return
        if mnem in ("fninit", "finit"):
            self.copy(va, ins)
            return
        if mnem in ("fnstsw", "fstsw"):
            # AH := low byte of the EFLAGS snapshot taken at the compare
            # (same layout as lahf); does not read or write EFLAGS.
            self.emit(b"\x64\x8A\x25\x00\x00\x00\x00", [(3, TEB_AH, 0)])
            return
        if mnem in ("fnstcw", "fstcw"):
            self.copy(va, ins)
            return
        if mnem == "fldcw":
            self.copy(va, ins)  # keep the real FPU CW in sync
            free = self._free_gp(mem)
            self.emit(E_.push_r(free))
            self.emit_mem(E_.mov_r_m(free, mem, esp_bias=4, width=2))
            # mov [data+D_SHADOW_CW], r16
            enc, doff = Mem(disp=0).encode(free)
            self.emit(b"\x66\x89" + enc, [(2 + doff, ABS32_DATA, D_SHADOW_CW)])
            self.emit(E_.pop_r(free))
            return

        if mnem in FLDCONST:
            self.emit_data_mem(E_.MOVSD_LOAD, X(d), FLDCONST[mnem])
            return

        if mnem == "fld":
            if mem is not None:
                if msz == 4:
                    self.emit_mem(E_.sse_rm(*E_.CVTSS2SD, X(d), mem))
                elif msz == 8:
                    self.emit_mem(E_.sse_rm(*E_.MOVSD_LOAD, X(d), mem))
                else:
                    raise TranslateError("fld size")
            else:
                i = sts[0] if sts else 0
                self.emit(E_.sse_rr(*E_.MOVSD_LOAD, X(d), X(t - i)))
            return

        if mnem == "fild":
            if msz == 4:
                self.emit_mem(E_.sse_rm(*E_.CVTSI2SD, X(d), mem))
            elif msz == 2:
                free = self._free_gp(mem)
                self.emit(E_.push_r(free))
                self.emit_mem(E_.movsx16_r_m(free, mem, esp_bias=4))
                self.emit(E_.sse_rr(*E_.CVTSI2SD, X(d), free))
                self.emit(E_.pop_r(free))
            elif msz == 8:
                self.emit_mem(E_.push_m(mem, esp_bias=0, disp_add=4))
                self.emit_mem(E_.push_m(mem, esp_bias=4))
                self.emit(b"\xE8\x00\x00\x00\x00", [(1, REL32_HELPER, H_I64TOD)])
                self.emit(E_.movsd_xmm_espmem(X(d), 0))
                self.emit(E_.lea_esp(8))
            else:
                raise TranslateError("fild size")
            return

        if mnem in ("fst", "fstp"):
            if mem is not None:
                if msz == 4:
                    self.emit(E_.sse_rr(*E_.CVTSD2SS, 7, X(t)))
                    self.emit_mem(E_.sse_rm(*E_.MOVSS_STORE, 7, mem))
                elif msz == 8:
                    self.emit_mem(E_.sse_rm(*E_.MOVSD_STORE, X(t), mem))
                else:
                    raise TranslateError("fst size")
            else:
                i = sts[0] if sts else 0
                if not (mnem == "fstp" and i == 0):
                    self.emit(E_.sse_rr(*E_.MOVSD_LOAD, X(t - i), X(t)))
            return

        if mnem in ("fist", "fistp"):
            if msz in (2, 4):
                free = self._free_gp(mem)
                self.emit(E_.push_r(free))
                # test byte [shadow_cw+1], 0x0C ; jnz Ltrunc
                enc, doff = Mem(disp=0).encode(0)  # F6 /0
                self.emit(b"\xF6" + enc + b"\x0C",
                          [(1 + doff, ABS32_DATA, D_SHADOW_CW + 1)])
                self.emit(b"\x75\x06")                       # jnz +6
                self.emit(E_.sse_rr(*E_.CVTSD2SI, free, X(t)))   # 4 bytes
                self.emit(b"\xEB\x04")                       # jmp +4
                self.emit(E_.sse_rr(*E_.CVTTSD2SI, free, X(t)))  # 4 bytes
                self.emit_mem(E_.mov_m_r(mem, free, esp_bias=4,
                                         width=2 if msz == 2 else 4))
                self.emit(E_.pop_r(free))
            elif msz == 8:
                free = self._free_gp(mem)
                self.emit(E_.push_r(free))
                self.emit(E_.lea_esp(-16))
                self.emit(E_.movsd_espmem_xmm(0, X(t)))
                self.emit(b"\xE8\x00\x00\x00\x00", [(1, REL32_HELPER, H_DTOI64)])
                self.emit(E_.mov_r_m(free, Mem(base=E_.ESP, disp=0))[0])
                self.emit_mem(E_.mov_m_r(mem, free, esp_bias=20))
                self.emit(E_.mov_r_m(free, Mem(base=E_.ESP, disp=4))[0])
                self.emit_mem(E_.mov_m_r(mem, free, esp_bias=20, disp_add=4))
                self.emit(E_.lea_esp(16))
                self.emit(E_.pop_r(free))
            else:
                raise TranslateError("fist size")
            return

        if mnem == "fxch":
            i = sts[-1] if sts else 1
            if i != 0:
                self.emit(E_.sse_rr(*E_.MOVSD_LOAD, 7, X(t)))
                self.emit(E_.sse_rr(*E_.MOVSD_LOAD, X(t), X(t - i)))
                self.emit(E_.sse_rr(*E_.MOVSD_LOAD, X(t - i), 7))
            return

        if mnem == "fchs":
            self.emit_data_mem(E_.XORPD, X(t), D_SIGNMASK)
            return
        if mnem == "fabs":
            self.emit_data_mem(E_.ANDPD, X(t), D_ABSMASK)
            return
        if mnem == "fsqrt":
            self.emit(E_.sse_rr(*E_.SQRTSD, X(t), X(t)))
            self.post_arith(X(t), is_div=True)  # sqrt(neg) births NaN: check
            return

        ARITH = {"fadd": E_.ADDSD, "fmul": E_.MULSD, "fsub": E_.SUBSD,
                 "fdiv": E_.DIVSD, "fsubr": E_.SUBSD, "fdivr": E_.DIVSD,
                 "faddp": E_.ADDSD, "fmulp": E_.MULSD, "fsubp": E_.SUBSD,
                 "fdivp": E_.DIVSD, "fsubrp": E_.SUBSD, "fdivrp": E_.DIVSD,
                 "fiadd": E_.ADDSD, "fimul": E_.MULSD, "fisub": E_.SUBSD,
                 "fidiv": E_.DIVSD, "fisubr": E_.SUBSD, "fidivr": E_.DIVSD}
        if mnem in ARITH:
            op2 = ARITH[mnem]
            is_div = "div" in mnem
            reverse = "r" in mnem.replace("f", "", 1)  # fsubr/fdivr/fsubrp/fdivrp/fisubr/fidivr
            if mem is not None:
                # rhs -> xmm7 (or direct mem form when possible)
                if mnem.startswith("fi"):
                    if msz == 4:
                        self.emit_mem(E_.sse_rm(*E_.CVTSI2SD, 7, mem))
                    elif msz == 2:
                        free = self._free_gp(mem)
                        self.emit(E_.push_r(free))
                        self.emit_mem(E_.movsx16_r_m(free, mem, esp_bias=4))
                        self.emit(E_.sse_rr(*E_.CVTSI2SD, 7, free))
                        self.emit(E_.pop_r(free))
                    else:
                        raise TranslateError("fiarith size")
                    rhs_in_7 = True
                elif msz == 4:
                    self.emit_mem(E_.sse_rm(*E_.CVTSS2SD, 7, mem))
                    rhs_in_7 = True
                elif msz == 8:
                    if not reverse:
                        self.emit_mem(E_.sse_rm(op2[0], op2[1], X(t), mem))
                        self.post_arith(X(t), is_div)
                        return
                    self.emit_mem(E_.sse_rm(*E_.MOVSD_LOAD, 7, mem))
                    rhs_in_7 = True
                else:
                    raise TranslateError("arith size")
                if not reverse:
                    self.emit(E_.sse_rr(op2[0], op2[1], X(t), 7))
                else:
                    # st0 = rhs OP st0
                    self.emit(E_.sse_rr(op2[0], op2[1], 7, X(t)))
                    self.emit(E_.sse_rr(*E_.MOVSD_LOAD, X(t), 7))
                self.post_arith(X(t), is_div)
                return
            # register forms
            pop = mnem.endswith("p") and mnem not in ("fsub", "fisub")  # *p forms
            if pop:
                i = sts[0] if sts else 1
                k = X(t - i)
                if mnem in ("faddp", "fmulp", "fsubp", "fdivp"):
                    self.emit(E_.sse_rr(op2[0], op2[1], k, X(t)))
                else:  # fsubrp/fdivrp: st(i) = st0 OP st(i)
                    self.emit(E_.sse_rr(*E_.MOVSD_LOAD, 7, X(t)))
                    self.emit(E_.sse_rr(op2[0], op2[1], 7, k))
                    self.emit(E_.sse_rr(*E_.MOVSD_LOAD, k, 7))
                self.post_arith(k, is_div)
                return
            # non-pop, two-reg (or implicit st0,st1)
            if len(sts) >= 2:
                dst_is_st0 = ins.operands[0].type == X86_OP_REG and \
                    ins.operands[0].reg == x86.X86_REG_ST0
                i = sts[1] if dst_is_st0 else sts[0]
            else:
                dst_is_st0, i = True, sts[0] if sts else 1
            k = X(t - i)
            if dst_is_st0:
                if not reverse:
                    self.emit(E_.sse_rr(op2[0], op2[1], X(t), k))
                else:   # st0 = st(i) OP st0
                    self.emit(E_.sse_rr(*E_.MOVSD_LOAD, 7, k))
                    self.emit(E_.sse_rr(op2[0], op2[1], 7, X(t)))
                    self.emit(E_.sse_rr(*E_.MOVSD_LOAD, X(t), 7))
            else:
                if not reverse:  # st(i) = st(i) OP st0
                    self.emit(E_.sse_rr(op2[0], op2[1], k, X(t)))
                else:            # st(i) = st0 OP st(i)
                    self.emit(E_.sse_rr(*E_.MOVSD_LOAD, 7, X(t)))
                    self.emit(E_.sse_rr(op2[0], op2[1], 7, k))
                    self.emit(E_.sse_rr(*E_.MOVSD_LOAD, k, 7))
            self.post_arith(X(t) if dst_is_st0 else k, is_div)
            return

        if mnem in ("fcom", "fcomp", "fcompp", "fucom", "fucomp", "fucompp",
                    "ficom", "ficomp", "ftst", "fcomi", "fcomip",
                    "fucomi", "fucomip"):
            if mnem == "ftst":
                self.emit_data_mem(E_.UCOMISD, X(t), D_ZERO)
            elif mem is not None:
                if mnem.startswith("fi"):
                    if msz == 4:
                        self.emit_mem(E_.sse_rm(*E_.CVTSI2SD, 7, mem))
                    elif msz == 2:
                        free = self._free_gp(mem)
                        self.emit(E_.push_r(free))
                        self.emit_mem(E_.movsx16_r_m(free, mem, esp_bias=4))
                        self.emit(E_.sse_rr(*E_.CVTSI2SD, 7, free))
                        self.emit(E_.pop_r(free))
                    else:
                        raise TranslateError("ficom size")
                    self.emit(E_.sse_rr(*E_.UCOMISD, X(t), 7))
                elif msz == 4:
                    self.emit_mem(E_.sse_rm(*E_.CVTSS2SD, 7, mem))
                    self.emit(E_.sse_rr(*E_.UCOMISD, X(t), 7))
                elif msz == 8:
                    self.emit_mem(E_.sse_rm(*E_.UCOMISD, X(t), mem))
                else:
                    raise TranslateError("fcom size")
            else:
                i = sts[-1] if sts else 1
                self.emit(E_.sse_rr(*E_.UCOMISD, X(t), X(t - i)))
            # snapshot EFLAGS to TEB so any later fnstsw can read this
            # compare's result regardless of intervening flag writers
            # (pushfd + pop dword fs:[TEB]; touches no registers)
            self.emit(b"\x9C")
            self.emit(b"\x64\x8F\x05\x00\x00\x00\x00", [(3, TEB_AH, 0)])
            return

        if mnem == "fsin":
            self.helper1(H_SIN, X(t)); return
        if mnem == "fcos":
            self.helper1(H_COS, X(t)); return
        if mnem == "f2xm1":
            self.helper1(H_2XM1, X(t)); return
        if mnem == "frndint":
            self.helper1(H_RNDINT, X(t)); return
        if mnem == "fptan":
            self.helper1(H_TAN, X(t))
            self.emit_data_mem(E_.MOVSD_LOAD, X(d), D_ONE)
            return
        if mnem == "fsincos":
            self.emit(E_.lea_esp(-16))
            self.emit(E_.movsd_espmem_xmm(0, X(t)))
            self.emit(b"\xE8\x00\x00\x00\x00", [(1, REL32_HELPER, H_SINCOS)])
            self.emit(E_.movsd_xmm_espmem(X(t), 0))
            self.emit(E_.movsd_xmm_espmem(X(d), 8))
            self.emit(E_.lea_esp(16))
            return
        if mnem == "fpatan":
            self.helper2(H_ATAN2, X(t - 1), X(t), X(t - 1))
            return
        if mnem == "fyl2x":
            self.helper2(H_YL2X, X(t - 1), X(t), X(t - 1))
            return
        if mnem == "fyl2xp1":
            self.helper2(H_YL2XP1, X(t - 1), X(t), X(t - 1))
            return
        if mnem == "fscale":
            self.helper2(H_SCALE, X(t), X(t - 1), X(t), out_from=0)
            return

        raise TranslateError(f"unhandled x87 {mnem}")

    def ci_call(self, name, d, t):
        if name == "sqrt":
            self.emit(E.sse_rr(*E.SQRTSD, t, t))
        elif name == "sin":
            self.helper1(H_SIN, t)
        elif name == "cos":
            self.helper1(H_COS, t)
        elif name == "tan":
            self.helper1(H_TAN, t)
        elif name == "atan2":
            self.helper2(H_ATAN2, t - 1, t, t - 1)
        elif name == "log":
            self.helper2(H_YL2X, t - 1, t, t - 1)
        else:
            raise TranslateError(f"ci {name}")

    # ------------- layout -------------
    def layout(self):
        """Resolve internal branches; produce (blob, fixups)."""
        offs = []
        pos = 0
        for it in self.items:
            offs.append(pos)
            if it.branch:
                kind = it.branch[0]
                pos += 6 if kind == "jcc" else 5
            else:
                pos += len(it.data)
        # rebuilt jump-table pointer arrays go at the end, 4-aligned
        pos = (pos + 3) & ~3
        table_off = {}
        for it in self.items:
            if it.jt is not None:
                table_off[it.jt] = pos
                pos += 4 * len(self.fi.jumptables[it.jt][1])
        total = pos
        # map original VA -> blob offset
        va2off = {va: offs[idx] for va, idx in self.va_index.items()}
        blob = bytearray()
        fixups = []
        for idx, it in enumerate(self.items):
            base = offs[idx]
            if it.branch:
                kind, cc, target = it.branch
                if kind == "jcc":
                    ins_len = 6
                    head = bytes([0x0F, cc])
                elif kind == "jmp":
                    ins_len = 5
                    head = b"\xE9"
                else:
                    ins_len = 5
                    head = b"\xE8"
                if target in va2off:
                    rel = va2off[target] - (base + ins_len)
                    blob += head + struct.pack("<i", rel)
                else:
                    blob += head + b"\x00\x00\x00\x00"
                    fixups.append((base + len(head), REL32_MODULE, target))
            else:
                blob += it.data
                for off, typ, arg in it.fixups:
                    if typ == ABS32_BLOB and it.jt is not None:
                        arg = table_off[it.jt]
                    fixups.append((base + off, typ, arg))
        blob += b"\x00" * (((len(blob) + 3) & ~3) - len(blob))
        for jt_va, toff in sorted(table_off.items(), key=lambda kv: kv[1]):
            for k, tgt in enumerate(self.fi.jumptables[jt_va][1]):
                blob += b"\x00\x00\x00\x00"
                fixups.append((toff + 4 * k, ABS32_BLOB, va2off[tgt]))
        assert len(blob) == total, (len(blob), total)
        return bytes(blob), fixups
