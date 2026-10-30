"""List code that reads/writes a global (by RVA), with a few instructions of context.

usage: py native/tools/xref_global.py 0x33ddaa9 [--ctx 6]
"""
import argparse

from pe import Image, fmt

ap = argparse.ArgumentParser()
ap.add_argument("rva", type=lambda s: int(s, 0))
ap.add_argument("--ctx", type=int, default=6, help="instructions of context after each hit")
args = ap.parse_args()

img = Image()
xrefs = img.rip_xrefs(args.rva)
print(f"{len(xrefs)} xrefs to rva {args.rva:#x}")
for x in xrefs:
    f = img.func_of(x)
    print(f"\n== {img.va(x):#x}  in func {img.va(f[0]):#x} (size {f[1] - f[0]:#x})" if f else f"\n== {img.va(x):#x}")
    for i, ins in enumerate(img.disasm(x, 16 * (args.ctx + 1))):
        if i > args.ctx:
            break
        print("   ", fmt(ins, img))
