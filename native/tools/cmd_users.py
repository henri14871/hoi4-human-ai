"""For each command class: find where its vtable is written (constructors), then which RTTI
classes reach those constructors (AI modules vs UI).

usage: py native/tools/cmd_users.py COrderNewFrontCommand COrderAssignCommand ... [--depth 4]
"""
import argparse
import re
import struct
from collections import deque

import capstone

from pe import Image

ap = argparse.ArgumentParser()
ap.add_argument("classes", nargs="+")
ap.add_argument("--depth", type=int, default=5)
args = ap.parse_args()
img = Image()
R = img.sections[".rdata"]
R0, R1 = R.VirtualAddress, R.VirtualAddress + R.Misc_VirtualSize

calls = {}
t, b = img.text, img.text_rva
for m in re.finditer(rb"\xe8", t):
    o = m.start()
    if o + 5 <= len(t):
        calls.setdefault(b + o + 5 + struct.unpack_from("<i", t, o + 1)[0], []).append(b + o)

vt_owner = {}  # function rva -> class (any vtable slot)


def build_vt_index():
    for m in re.finditer(rb"\x01\x00\x00\x00\x00\x00\x00\x00", img.data[R0:R1]):
        pass


def col_name(col):
    if not (R0 <= col < R1):
        return None
    sig, off, _, td, _, me = struct.unpack_from("<IIIIII", img.data, col)
    if sig != 1 or me != col:
        return None
    n = img.read_cstr(td + 16)
    return n[4:-2] if n.startswith(".?AV") else None


# index every vtable: a qword pointing at a COL followed by code pointers
for off in range(R0, R1 - 16, 8):
    p = struct.unpack_from("<Q", img.data, off)[0] - img.base
    name = col_name(p) if R0 <= p < R1 else None
    if not name:
        continue
    i = off + 8
    while i < R1:
        q = struct.unpack_from("<Q", img.data, i)[0] - img.base
        if not (img.text_rva <= q < img.text_rva + len(img.text)):
            break
        vt_owner.setdefault(q, name)
        i += 8


def func_start(rva):
    f = img.primary_func(rva)
    if f:
        return f[0]
    s = rva
    while s > 0 and not (img.data[s - 1] == 0xCC and img.data[s - 2] == 0xCC):
        s -= 1
    return s


def owners_of(fn):
    found, seen, q = {}, set(), deque([(fn, 0)])
    while q:
        f, d = q.popleft()
        if f in seen:
            continue
        seen.add(f)
        if f in vt_owner:
            found.setdefault(vt_owner[f], d)
            continue
        if d < args.depth:
            for c in calls.get(f, []):
                q.append((func_start(c), d + 1))
    return found


for cls in args.classes:
    td = img.find_bytes(f".?AV{cls}@@".encode() + b"\0")
    if not td:
        print(cls, "not found"); continue
    td = td[0] - 16
    vts = []
    for m in re.finditer(re.escape(struct.pack("<I", td)), img.data[R0:R1]):
        col = R0 + m.start() - 12
        if col_name(col) and struct.unpack_from("<I", img.data, col + 4)[0] == 0:
            for v in re.finditer(re.escape(struct.pack("<Q", img.va(col))), img.data[R0:R1]):
                vts.append(R0 + v.start() + 8)
    if not vts:
        print(cls, "no vtable"); continue
    sites = img.rip_xrefs(vts[0])
    ctors = sorted({func_start(s) for s in sites})
    agg = {}
    for c in ctors:
        for k, d in owners_of(c).items():
            if k != cls:
                agg[k] = min(d, agg.get(k, 99))
    ai = sorted(k for k in agg if "AI" in k)
    other = sorted(k for k in agg if "AI" not in k)
    print(f"{cls}: {len(ctors)} construct sites | AI: {', '.join(ai) or '-'} | other: {', '.join(other[:6]) or '-'}")
