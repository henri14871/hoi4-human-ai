"""List profiler scope labels: strings loaded into r8 just before a call to the profiler.

usage: py native/tools/profiler_labels.py [--profiler 0x142551ce0] [--grep REGEX]
Prints: call-site VA, containing function VA, label.
"""
import argparse
import re
import struct

from pe import Image

ap = argparse.ArgumentParser()
ap.add_argument("--profiler", type=lambda s: int(s, 0), default=0x142551CE0)
ap.add_argument("--grep", default="")
args = ap.parse_args()

img = Image()
t, base = img.text, img.text_rva
target = args.profiler - img.base
rx = re.compile(args.grep, re.I) if args.grep else None
for m in re.finditer(rb"\xe8", t):
    o = m.start()
    if o + 5 > len(t) or base + o + 5 + struct.unpack_from("<i", t, o + 1)[0] != target:
        continue
    # look back up to 24 bytes for lea r8, [rip+disp32] (4C 8D 05)
    for back in range(7, 32):
        s = o - back
        if t[s:s + 3] == b"\x4c\x8d\x05":
            lbl = img.read_cstr(base + s + 7 + struct.unpack_from("<i", t, s + 3)[0], 120)
            if not rx or rx.search(lbl):
                f = img.func_of(base + o)
                print(f"{img.va(base + o):#x}  func {img.va(f[0]) if f else 0:#x}  {lbl}")
            break
