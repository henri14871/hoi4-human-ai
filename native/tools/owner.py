"""Name the class that owns a function, via MSVC RTTI on vtables that point at it.

If the function is not a virtual, walk up direct callers (E8 rel32) until one is.

usage: py native/tools/owner.py 0x140eb41f0 [0x... ...] [--depth 3]
"""
import argparse
import re
import struct

from pe import Image

ap = argparse.ArgumentParser()
ap.add_argument("funcs", nargs="+", type=lambda s: int(s, 0), help="function VAs")
ap.add_argument("--depth", type=int, default=3)
args = ap.parse_args()

img = Image()
rdata = img.sections[".rdata"]
R0, R1 = rdata.VirtualAddress, rdata.VirtualAddress + rdata.Misc_VirtualSize


def col_name(col_rva):
    if not (R0 <= col_rva < R1):
        return None
    sig, _, _, td, _, self_rva = struct.unpack_from("<IIIIII", img.data, col_rva)
    if sig != 1 or self_rva != col_rva:
        return None
    name = img.read_cstr(td + 16)
    return name if name.startswith(".?A") else None


def vtable_owners(func_va):
    """[(class, slot)] for each vtable slot holding func_va."""
    out = []
    for m in re.finditer(re.escape(struct.pack("<Q", func_va)), img.data[R0:R1]):
        slot = R0 + m.start()
        if slot % 8:
            continue
        p = slot
        for i in range(400):  # walk back to the vtable start
            prev = struct.unpack_from("<Q", img.data, p - 8)[0]
            name = col_name(prev - img.base) if prev > img.base else None
            if name:
                out.append((name, (slot - p) // 8))
                break
            if not (img.text_rva <= prev - img.base < img.text_rva + len(img.text)):
                break
            p -= 8
    return out


_calls = None


def callers(func_rva):
    global _calls
    if _calls is None:
        _calls = {}
        t, base = img.text, img.text_rva
        for m in re.finditer(rb"\xe8", t):
            o = m.start()
            if o + 5 > len(t):
                break
            tgt = base + o + 5 + struct.unpack_from("<i", t, o + 1)[0]
            _calls.setdefault(tgt, []).append(base + o)
    res = set()
    for c in _calls.get(func_rva, []):
        f = img.primary_func(c)
        if f:
            res.add(f[0])
    return sorted(res)


def describe(func_rva, depth, indent=""):
    owners = vtable_owners(img.va(func_rva))
    if owners:
        for n, s in owners[:4]:
            print(f"{indent}{img.va(func_rva):#x}: vtable slot {s} of {n}")
        return
    cs = callers(func_rva)
    print(f"{indent}{img.va(func_rva):#x}: not virtual, {len(cs)} direct callers")
    if depth > 0:
        for c in cs[:6]:
            describe(c, depth - 1, indent + "  ")


for f in args.funcs:
    fr = img.primary_func(f - img.base)
    describe(fr[0] if fr else f - img.base, args.depth)
    print()
