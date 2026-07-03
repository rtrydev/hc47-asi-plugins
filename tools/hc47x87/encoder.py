"""Minimal x86-32 encoder for the SSE2 replacement sequences."""
import struct
from capstone import x86

# GP register numbers
EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI = range(8)

CS_GP = {
    x86.X86_REG_EAX: EAX, x86.X86_REG_ECX: ECX, x86.X86_REG_EDX: EDX,
    x86.X86_REG_EBX: EBX, x86.X86_REG_ESP: ESP, x86.X86_REG_EBP: EBP,
    x86.X86_REG_ESI: ESI, x86.X86_REG_EDI: EDI,
}

SEG_PREFIX = {
    x86.X86_REG_ES: 0x26, x86.X86_REG_CS: 0x2E, x86.X86_REG_SS: 0x36,
    x86.X86_REG_DS: 0x3E, x86.X86_REG_FS: 0x64, x86.X86_REG_GS: 0x65,
}


class Mem:
    """Decoded memory operand, re-encodable with a chosen /reg field."""

    def __init__(self, base=None, index=None, scale=1, disp=0, seg=None):
        self.base = base      # GP number or None
        self.index = index    # GP number or None
        self.scale = scale
        self.disp = disp
        self.seg = seg        # capstone seg reg id or 0/None

    @classmethod
    def from_op(cls, op):
        m = op.mem
        base = CS_GP.get(m.base) if m.base else None
        index = CS_GP.get(m.index) if m.index else None
        if (m.base and base is None) or (m.index and index is None):
            raise ValueError("non-GP addressing")
        return cls(base, index, m.scale, m.disp & 0xFFFFFFFF,
                   m.segment or None)

    def uses(self, reg):
        return self.base == reg or self.index == reg

    def prefix(self):
        if not self.seg:
            return b""
        default_ss = self.base in (ESP, EBP)
        want = SEG_PREFIX.get(self.seg)
        if want is None:
            raise ValueError("weird segment")
        if (default_ss and want == 0x36) or (not default_ss and want == 0x3E):
            return b""
        return bytes([want])

    def encode(self, reg_field, esp_bias=0, disp_add=0):
        """Return (bytes, disp32_offset_or_None). disp32 offset is relative
        to the start of the returned bytes (excluding segment prefix)."""
        disp = (self.disp + disp_add) & 0xFFFFFFFF
        if self.base == ESP:
            disp = (disp + esp_bias) & 0xFFFFFFFF
        b = self.base
        out = bytearray()
        disp_off = None

        if b is None and self.index is None:
            # absolute disp32
            out.append((reg_field << 3) | 0x05)
            disp_off = len(out)
            out += struct.pack("<I", disp)
            return bytes(out), disp_off

        sdisp = disp if disp < 0x80000000 else disp - 0x100000000
        need_sib = (self.index is not None) or (b == ESP)

        if self.index is None and not need_sib:
            # simple [base+disp]
            if sdisp == 0 and b != EBP:
                out.append(0x00 | (reg_field << 3) | b)
            elif -128 <= sdisp <= 127:
                out.append(0x40 | (reg_field << 3) | b)
                out += struct.pack("<b", sdisp)
            else:
                out.append(0x80 | (reg_field << 3) | b)
                disp_off = 1
                out += struct.pack("<I", disp)
            return bytes(out), disp_off

        # SIB forms
        scale_bits = {1: 0, 2: 1, 4: 2, 8: 3}[self.scale]
        idx = self.index if self.index is not None else 0b100
        if idx == ESP and self.index is not None:
            raise ValueError("esp as index")
        if b is None:
            # [index*scale + disp32]
            out.append(0x00 | (reg_field << 3) | 0x04)
            out.append((scale_bits << 6) | (idx << 3) | 0b101)
            disp_off = 2
            out += struct.pack("<I", disp)
            return bytes(out), disp_off
        if sdisp == 0 and b != EBP:
            out.append(0x00 | (reg_field << 3) | 0x04)
            out.append((scale_bits << 6) | (idx << 3) | b)
        elif -128 <= sdisp <= 127:
            out.append(0x40 | (reg_field << 3) | 0x04)
            out.append((scale_bits << 6) | (idx << 3) | b)
            out += struct.pack("<b", sdisp)
        else:
            out.append(0x80 | (reg_field << 3) | 0x04)
            out.append((scale_bits << 6) | (idx << 3) | b)
            disp_off = 2
            out += struct.pack("<I", disp)
        return bytes(out), disp_off


def modrm_rr(reg, rm):
    return bytes([0xC0 | (reg << 3) | rm])


# --- SSE scalar ops (prefix, 0F, opcode) ---
def sse_rr(prefix, opcode, reg, rm):
    return bytes([prefix, 0x0F, opcode]) + modrm_rr(reg, rm)


def sse_rm(prefix, opcode, reg, mem, esp_bias=0, disp_add=0):
    """Returns (bytes, disp32_offset_or_None relative to full bytes)."""
    pfx = mem.prefix()
    enc, doff = mem.encode(reg, esp_bias, disp_add)
    head = pfx + bytes([prefix, 0x0F, opcode])
    return head + enc, (len(head) + doff) if doff is not None else None


MOVSD_LOAD = (0xF2, 0x10)   # movsd xmm, m64/xmm
MOVSD_STORE = (0xF2, 0x11)  # movsd m64/xmm, xmm
MOVSS_LOAD = (0xF3, 0x10)
MOVSS_STORE = (0xF3, 0x11)
CVTSS2SD = (0xF3, 0x5A)
CVTSD2SS = (0xF2, 0x5A)
CVTSI2SD = (0xF2, 0x2A)
CVTTSD2SI = (0xF2, 0x2C)
CVTSD2SI = (0xF2, 0x2D)
ADDSD = (0xF2, 0x58)
MULSD = (0xF2, 0x59)
SUBSD = (0xF2, 0x5C)
DIVSD = (0xF2, 0x5E)
SQRTSD = (0xF2, 0x51)
UCOMISD = (0x66, 0x2E)
ANDPD = (0x66, 0x54)
XORPD = (0x66, 0x57)


def push_r(r):
    return bytes([0x50 + r])


def pop_r(r):
    return bytes([0x58 + r])


def lea_esp(delta):
    """lea esp, [esp+delta] — ESP adjust without touching EFLAGS."""
    if -128 <= delta <= 127:
        return bytes([0x8D, 0x64, 0x24, delta & 0xFF])
    return bytes([0x8D, 0xA4, 0x24]) + struct.pack("<i", delta)


def mov_r_m(r, mem, esp_bias=0, disp_add=0, width=4):
    pfx = mem.prefix() + (b"\x66" if width == 2 else b"")
    opc = 0x8B
    enc, doff = mem.encode(r, esp_bias, disp_add)
    head = pfx + bytes([opc])
    return head + enc, (len(head) + doff) if doff is not None else None


def mov_m_r(mem, r, esp_bias=0, disp_add=0, width=4):
    pfx = mem.prefix() + (b"\x66" if width == 2 else b"")
    opc = 0x89
    enc, doff = mem.encode(r, esp_bias, disp_add)
    head = pfx + bytes([opc])
    return head + enc, (len(head) + doff) if doff is not None else None


def movsx16_r_m(r, mem, esp_bias=0, disp_add=0):
    pfx = mem.prefix()
    enc, doff = mem.encode(r, esp_bias, disp_add)
    head = pfx + bytes([0x0F, 0xBF])
    return head + enc, (len(head) + doff) if doff is not None else None


def push_m(mem, esp_bias=0, disp_add=0):
    pfx = mem.prefix()
    enc, doff = mem.encode(6, esp_bias, disp_add)  # FF /6
    head = pfx + bytes([0xFF])
    return head + enc, (len(head) + doff) if doff is not None else None


# esp-relative fixed slots (no flags impact anywhere in these)
def movsd_xmm_espmem(x, off):
    m = Mem(base=ESP, disp=off)
    b, _ = sse_rm(*MOVSD_LOAD, x, m)
    return b


def movsd_espmem_xmm(off, x):
    m = Mem(base=ESP, disp=off)
    b, _ = sse_rm(*MOVSD_STORE, x, m)
    return b


FSTP_QWORD_ESP = bytes([0xDD, 0x1C, 0x24])  # fstp qword [esp]
FLD_QWORD_ESP = bytes([0xDD, 0x04, 0x24])   # fld qword [esp]
LAHF = b"\x9F"
CDQ = b"\x99"
