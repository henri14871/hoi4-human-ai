"""Find code that references a string, and dump the surrounding function.

usage: py native/tools/xref_string.py "human_ai" [--dump N]
"""
import argparse

from pe import Image, fmt

ap = argparse.ArgumentParser()
ap.add_argument("string")
ap.add_argument("--dump", type=int, default=0, help="disassemble up to N bytes of each referencing function")
args = ap.parse_args()

img = Image()
for srva in img.find_cstring(args.string):
    print(f"string @ rva {srva:#x} (va {img.va(srva):#x})")
    for x in img.rip_xrefs(srva):
        f = img.func_of(x)
        fs = f"func {img.va(f[0]):#x}-{img.va(f[1]):#x}" if f else "no pdata func"
        print(f"  xref @ {img.va(x):#x}  ({fs})")
        if args.dump and f:
            for ins in img.disasm(f[0], min(args.dump, f[1] - f[0])):
                mark = ">>" if ins.address == img.va(x) else "  "
                print("   ", mark, fmt(ins, img))
