"""PE module loading: sections, relocations, exports, disassembly cache."""
import bisect
import pefile
import capstone
from capstone import x86

IMAGE_SCN_MEM_EXECUTE = 0x20000000


class Module:
    def __init__(self, path):
        self.path = path
        self.pe = pefile.PE(path)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        # Executable ranges: list of (va_start, va_end, bytes)
        self.text = []
        for s in self.pe.sections:
            if s.Characteristics & IMAGE_SCN_MEM_EXECUTE:
                va = self.base + s.VirtualAddress
                data = s.get_data()[: s.Misc_VirtualSize]
                self.text.append((va, va + len(data), data))
        self.sections = []
        for s in self.pe.sections:
            va = self.base + s.VirtualAddress
            data = s.get_data()[: s.Misc_VirtualSize]
            self.sections.append((va, va + len(data), data))
        self.entry = self.base + self.pe.OPTIONAL_HEADER.AddressOfEntryPoint
        self.exports = []
        if hasattr(self.pe, "DIRECTORY_ENTRY_EXPORT"):
            for e in self.pe.DIRECTORY_ENTRY_EXPORT.symbols:
                if e.address:
                    self.exports.append(self.base + e.address)
        # Relocation VAs (HIGHLOW): addresses of 32-bit absolute pointers.
        self.relocs = set()
        if hasattr(self.pe, "DIRECTORY_ENTRY_BASERELOC"):
            for block in self.pe.DIRECTORY_ENTRY_BASERELOC:
                for entry in block.entries:
                    if entry.type == 3:  # IMAGE_REL_BASED_HIGHLOW
                        self.relocs.add(self.base + entry.rva)
        self._relocs_sorted = sorted(self.relocs)
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self._insn_cache = {}

    def in_text(self, va):
        return any(s <= va < e for s, e, _ in self.text)

    def read(self, va, n):
        for s, e, data in self.text:
            if s <= va < e:
                return data[va - s : va - s + n]
        return b""

    def read_any(self, va, n):
        for s, e, data in self.sections:
            if s <= va < e:
                return data[va - s : va - s + n]
        return b""

    def insn_at(self, va):
        """Decode single instruction at va (cached). Returns None on failure."""
        ins = self._insn_cache.get(va)
        if ins is None and va not in self._insn_cache:
            raw = self.read(va, 16)
            ins = next(self.md.disasm(raw, va, 1), None) if raw else None
            self._insn_cache[va] = ins
        return ins

    def relocs_in(self, va, size):
        """Reloc VAs falling inside [va, va+size)."""
        i = bisect.bisect_left(self._relocs_sorted, va)
        out = []
        while i < len(self._relocs_sorted) and self._relocs_sorted[i] < va + size:
            out.append(self._relocs_sorted[i])
            i += 1
        return out
