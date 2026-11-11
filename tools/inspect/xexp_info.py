import struct, sys
sys.stdout.reconfigure(encoding="utf-8")
def ver(v):
    return f"{v>>28}.{(v>>24)&0xF}.{(v>>8)&0xFFFF}.{v&0xFF}"
for p in sys.argv[1:]:
    d = open(p, "rb").read(0x10000)
    magic, flags, peoff, _, secoff, n = struct.unpack(">4sIIIII", d[:24])
    kinds = [k for k, m in ((1, "title"), (2, "title-patch"), (4, "patch-delta"), (8, "patch-full")) if flags & k]
    out = f"{p}: {magic.decode()} flags={flags:#x}"
    for i in range(n):
        k, v = struct.unpack(">II", d[24 + i * 8:32 + i * 8])
        if k == 0x00040006:
            mid, v1, v2, tid = struct.unpack(">IIII", d[v:v + 16])
            out += f" version={ver(v1)} base={ver(v2)} title={tid:08X}"
        if k == 0x000005FF:
            out += " [delta-patch descriptor]"
    print(out)
