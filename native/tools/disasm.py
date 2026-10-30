"""Disassemble around an address, decoding from its function start so alignment is right.

usage: py native/tools/disasm.py 0x1401f38e2 [--before 40] [--after 40]
       py native/tools/disasm.py 0x1402c1a40 --raw 0x60    (no pdata: decode from the address)
"""
import argparse

from pe import Image, fmt

ap = argparse.ArgumentParser()
ap.add_argument("va", type=lambda s: int(s, 0))
ap.add_argument("--before", type=int, default=30, help="instructions before")
ap.add_argument("--after", type=int, default=30, help="instructions after")
ap.add_argument("--raw", type=lambda s: int(s, 0), default=0, help="decode N bytes from va directly")
args = ap.parse_args()

img = Image()
rva = args.va - img.base
if args.raw:
    for ins in img.disasm(rva, args.raw):
        print(fmt(ins, img))
    raise SystemExit
f = img.func_of(rva)
if not f:
    raise SystemExit("no pdata function contains this address; use --raw")
inss = img.disasm(f[0], f[1] - f[0])
idx = next(i for i, ins in enumerate(inss) if ins.address >= args.va)
print(f"; func {img.va(f[0]):#x}-{img.va(f[1]):#x}")
for ins in inss[max(0, idx - args.before):idx + args.after]:
    print(">>" if ins.address == args.va else "  ", fmt(ins, img))
