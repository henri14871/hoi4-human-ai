"""Shared helpers for static analysis of hoi4.exe (x64 PE)."""
import bisect
import os
import re
import struct
import sys

import capstone
import pefile

DEFAULT_EXE = r"E:\SteamLibrary\steamapps\common\Hearts of Iron IV\hoi4.exe"


class Image:
    def __init__(self, path=None):
        self.path = path or os.environ.get("HOI4_EXE", DEFAULT_EXE)
        self.pe = pefile.PE(self.path, fast_load=True)
        self.pe.parse_data_directories(directories=[
            pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
            pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"],
        ])
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.data = self.pe.get_memory_mapped_image()
        self.sections = {s.Name.rstrip(b"\0").decode(): s for s in self.pe.sections}
        text = self.sections[".text"]
        self.text_rva = text.VirtualAddress
        self.text = self.data[self.text_rva:self.text_rva + text.Misc_VirtualSize]
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.md.detail = True
        # Function boundaries from .pdata (RUNTIME_FUNCTION entries).
        funcs = sorted({(e.struct.BeginAddress, e.struct.EndAddress)
                        for e in getattr(self.pe, "DIRECTORY_ENTRY_EXCEPTION", [])})
        self.func_starts = [b for b, _ in funcs]
        self.func_ends = [e for _, e in funcs]

    def func_of(self, rva):
        i = bisect.bisect_right(self.func_starts, rva) - 1
        if i >= 0 and rva < self.func_ends[i]:
            return self.func_starts[i], self.func_ends[i]
        return None

    def primary_func(self, rva):
        """Like func_of, but follows chained unwind info from a split-off chunk to its parent."""
        f = self.func_of(rva)
        for _ in range(8):
            if not f:
                return None
            unwind = self._unwind_of(f[0])
            if unwind is None:
                return f
            flags = self.data[unwind] >> 3
            if not flags & 0x4:  # UNW_FLAG_CHAININFO
                return f
            count = self.data[unwind + 2]
            chained = unwind + 4 + ((count + 1) & ~1) * 2
            begin = struct.unpack_from("<I", self.data, chained)[0]
            f = self.func_of(begin)
        return f

    def _unwind_of(self, begin):
        if not hasattr(self, "_unwind"):
            self._unwind = {e.struct.BeginAddress: e.struct.UnwindData
                            for e in getattr(self.pe, "DIRECTORY_ENTRY_EXCEPTION", [])}
        return self._unwind.get(begin)

    def find_bytes(self, needle):
        return [m.start() for m in re.finditer(re.escape(needle), self.data)]

    def find_cstring(self, s):
        """RVAs of NUL-terminated occurrences of s (also preceded by NUL)."""
        b = s.encode() + b"\0"
        return [r for r in self.find_bytes(b) if r == 0 or self.data[r - 1] == 0]

    def rip_xrefs(self, target_rva):
        """Code locations whose RIP-relative disp32 resolves to target_rva.

        Scans for any 4-byte window where (window_end_rva + disp) == target, then
        confirms by disassembling the instruction that contains it.
        """
        text, base = self.text, self.text_rva
        out = set()
        # A RIP-relative operand is a modrm byte with mod=00, rm=101 followed by disp32;
        # disp is relative to the end of the instruction (disp + any trailing immediate).
        for m in re.finditer(rb"(?=[\x05\x0d\x15\x1d\x25\x2d\x35\x3d](....))", text, re.S):
            disp_off = m.start() + 1
            disp = struct.unpack_from("<i", text, disp_off)[0]
            for trail in (0, 1, 2, 4):
                if base + disp_off + 4 + trail + disp == target_rva:
                    out.add((m.start(), trail))
        confirmed = []
        for modrm_off, _ in sorted(out):
            for back in range(1, 5):
                start = modrm_off - back
                ins = next(self.md.disasm(text[start:start + 16], self.base + base + start, 1), None)
                if ins is None:
                    continue
                for op in ins.operands:
                    if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                        if ins.address + ins.size + op.mem.disp - self.base == target_rva:
                            confirmed.append(base + start)
                            break
                else:
                    continue
                break
        return sorted(set(confirmed))

    def disasm(self, rva, size):
        return list(self.md.disasm(self.data[rva:rva + size], self.base + rva))

    def va(self, rva):
        return self.base + rva

    def read_cstr(self, rva, maxlen=256):
        end = self.data.find(b"\0", rva, rva + maxlen)
        return self.data[rva:end].decode("latin-1")


def fmt(ins, img):
    s = f"{ins.address:016x}  {ins.mnemonic:8} {ins.op_str}"
    for op in ins.operands:
        if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
            tgt = ins.address + ins.size + op.mem.disp - img.base
            if 0 <= tgt < len(img.data):
                txt = img.read_cstr(tgt, 64)
                if len(txt) >= 3 and txt.isprintable():
                    s += f'    ; "{txt}"'
                else:
                    s += f"    ; rva {tgt:#x}"
    return s
