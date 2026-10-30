"""Find a function's start (walking back to int3/padding when there is no pdata) and list
its direct callers with the RTTI owner of each caller, up a few levels.

usage: py native/tools/callers.py 0x1411d0d51 [--depth 3]
"""
import argparse
import subprocess
import sys

from pe import Image

ap = argparse.ArgumentParser()
ap.add_argument("va", type=lambda s: int(s, 0))
ap.add_argument("--depth", type=int, default=3)
args = ap.parse_args()

img = Image()
rva = args.va - img.base
f = img.primary_func(rva)
if f:
    start = f[0]
else:
    start = rva
    while start > 0 and not (img.data[start - 1] == 0xCC and img.data[start - 2] == 0xCC):
        start -= 1
print(f"function start {img.va(start):#x}")
subprocess.run([sys.executable, "owner.py", hex(img.va(start)), "--depth", str(args.depth)])
