import argparse
import struct

import capstone

BASE = 0x82000000

ap = argparse.ArgumentParser()
ap.add_argument("image")
ap.add_argument("addr", type=lambda s: int(s, 16))
ap.add_argument("count", type=int, nargs="?", default=24)
ap.add_argument("--before", type=int, default=0)
a = ap.parse_args()

data = open(a.image, "rb").read()
md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
start = a.addr - 4 * a.before
for i in range(a.before + a.count):
    ea = start + 4 * i
    word = data[ea - BASE: ea - BASE + 4]
    ins = next(md.disasm(word, ea), None)
    text = f"{ins.mnemonic} {ins.op_str}" if ins else f".long 0x{struct.unpack('>I', word)[0]:08X}"
    mark = ">>" if ea == a.addr else "  "
    print(f"{mark} {ea:08X}: {word.hex()}  {text}")
