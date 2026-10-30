"""Summarise a function: direct callees, virtual call offsets, referenced strings.

usage: py native/tools/callees.py 0x140e0abb0 [--strings-only]
"""
import argparse

import capstone

from pe import Image

ap = argparse.ArgumentParser()
ap.add_argument("va", type=lambda s: int(s, 0))
args = ap.parse_args()

img = Image()
f = img.func_of(args.va - img.base)
if not f:
    raise SystemExit("no pdata function here")
print(f"; func {img.va(f[0]):#x}-{img.va(f[1]):#x} size {f[1] - f[0]:#x}")
for ins in img.disasm(f[0], f[1] - f[0]):
    note = None
    if ins.mnemonic == "call":
        op = ins.operands[0]
        if op.type == capstone.x86.X86_OP_IMM:
            note = f"call {op.imm:#x}"
        elif op.type == capstone.x86.X86_OP_MEM and op.mem.base != capstone.x86.X86_REG_RIP:
            note = f"vcall [{ins.reg_name(op.mem.base)}+{op.mem.disp:#x}]"
    for op in ins.operands:
        if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
            tgt = ins.address + ins.size + op.mem.disp - img.base
            if 0 <= tgt < len(img.data):
                s = img.read_cstr(tgt, 80)
                if len(s) >= 4 and s.isprintable():
                    note = f'str "{s}"'
    if note:
        print(f"{ins.address:#x}  {note}")
