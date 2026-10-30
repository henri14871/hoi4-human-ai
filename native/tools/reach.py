"""From each AI module's update slots, walk direct calls (and calls through the module's
own vtable) and report which profiler-labelled / string-referencing functions are reached.

usage: py native/tools/reach.py [--depth 3] [--grep REGEX]
"""
import argparse
import re
import struct
from collections import deque

import capstone

from pe import Image

ap = argparse.ArgumentParser()
ap.add_argument("--depth", type=int, default=3)
ap.add_argument("--funcs", nargs="*", type=lambda s: int(s, 0), help="walk from these VAs instead of module vtables")
ap.add_argument("--grep", default=r"nav|air|fleet|ship|taskforce|carrier|convoy|front|army|division|unit|land|wing|theat|invasion|mission|plane|port|naval")
args = ap.parse_args()
rx = re.compile(args.grep, re.I)

img = Image()
CLASSES = {
    "CAIForeignMinister": 0x142a26178, "CAIInteriorMinister": 0x142a26848,
    "CAIMilitaryMinister": 0x142a05320, "CAIPoliticalMinister": 0x142a274c0,
    "CAIGeneral": 0x142a03e78, "CAIBaseGeneral": 0x142a97b10,
    "CAIVolunteerGeneral": 0x142aa9a90, "CCryptologyAI": 0x142a25f98,
}
SLOTS = range(11, 18)

_cache = {}


def scan_func(rva):
    """(direct call targets, strings referenced) for the function containing rva."""
    f = img.func_of(rva)
    if not f:
        return [], []
    if f[0] in _cache:
        return _cache[f[0]]
    calls, strs = [], []
    for ins in img.md.disasm(img.data[f[0]:f[1]], img.va(f[0])):
        if ins.mnemonic == "call" and ins.operands and ins.operands[0].type == capstone.x86.X86_OP_IMM:
            calls.append(ins.operands[0].imm - img.base)
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                tgt = ins.address + ins.size + op.mem.disp - img.base
                if 0 <= tgt < len(img.data):
                    s = img.read_cstr(tgt, 100)
                    if 4 <= len(s) and s.isprintable() and rx.search(s):
                        strs.append(s)
    _cache[f[0]] = (calls, strs)
    return _cache[f[0]]


if args.funcs:
    roots = {f"{f:#x}": [f - img.base] for f in args.funcs}
else:
    roots = {c: [struct.unpack_from("<Q", img.data, vt - img.base + 8 * s)[0] - img.base for s in SLOTS]
             for c, vt in CLASSES.items()}

for cls, starts in roots.items():
    seen, found = set(), {}
    q = deque((p, 0) for p in starts)
    while q:
        rva, d = q.popleft()
        f = img.func_of(rva)
        if not f or f[0] in seen:
            continue
        seen.add(f[0])
        calls, strs = scan_func(f[0])
        for s in strs:
            found.setdefault(s, d)
        if d < args.depth:
            for c in calls:
                q.append((c, d + 1))
    labels = sorted(found, key=lambda s: (found[s], s))
    print(f"== {cls}: {len(seen)} functions reached")
    print("   " + " | ".join(f"{s}" for s in labels[:40]))
