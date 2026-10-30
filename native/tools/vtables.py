"""Dump MSVC vtables for classes by RTTI name and show which slots each overrides.

usage: py native/tools/vtables.py CAIModule CAIForeignMinister ... [--slots 40]
"""
import argparse
import re
import struct

from pe import Image

ap = argparse.ArgumentParser()
ap.add_argument("classes", nargs="+")
ap.add_argument("--slots", type=int, default=40)
args = ap.parse_args()

img = Image()
rdata = img.sections[".rdata"]
R0, R1 = rdata.VirtualAddress, rdata.VirtualAddress + rdata.Misc_VirtualSize
T0, T1 = img.text_rva, img.text_rva + len(img.text)


def vtables_of(cls):
    name = f".?AV{cls}@@".encode() + b"\0"
    out = []
    for nm in img.find_bytes(name):
        td = nm - 16
        # COL: sig=1, offset, cdOffset, pTypeDescriptor(rva), pClassHierarchy(rva), pSelf(rva)
        for m in re.finditer(re.escape(struct.pack("<I", td)), img.data[R0:R1]):
            col = R0 + m.start() - 12
            if col % 4:
                continue
            sig, off, _, tdr, _, selfr = struct.unpack_from("<IIIIII", img.data, col)
            if sig != 1 or tdr != td or selfr != col:
                continue
            for v in re.finditer(re.escape(struct.pack("<Q", img.va(col))), img.data[R0:R1]):
                vt = R0 + v.start() + 8
                if vt % 8 == 0:
                    out.append((off, vt))
    return out


def slots(vt, n):
    res = []
    for i in range(n):
        p = struct.unpack_from("<Q", img.data, vt + 8 * i)[0] - img.base
        if not (T0 <= p < T1):
            break
        res.append(p)
    return res


tables = {}
for c in args.classes:
    for off, vt in vtables_of(c):
        s = slots(vt, args.slots)
        tables[(c, off)] = s
        print(f"{c} (subobject offset {off:#x}) vtable @ {img.va(vt):#x}, {len(s)} slots")

base = next((v for (c, o), v in tables.items() if c == args.classes[0] and o == 0), None)
if base:
    print(f"\nslot table (= same as {args.classes[0]}, * = overridden)")
    for (c, o), s in tables.items():
        if o:
            continue
        row = " ".join(("." if i < len(base) and p == base[i] else "*") for i, p in enumerate(s))
        print(f"{c:28} {row}")
    print("\nper-slot addresses:")
    for (c, o), s in tables.items():
        if o:
            continue
        print(c, " ".join(f"{i}:{img.va(p):#x}" for i, p in enumerate(s) if not (i < len(base) and p == base[i])))
