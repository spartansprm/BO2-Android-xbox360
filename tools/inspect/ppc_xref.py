import argparse
import struct

BASE = 0x82000000
TEXT = (0x82160000, 0x82160000 + 0x7DEB24)
WINDOW = 12

OPS = {14: "addi", 24: "ori", 32: "lwz", 34: "lbz", 36: "stw", 38: "stb", 40: "lhz",
       44: "sth", 48: "lfs", 50: "lfd", 52: "stfs", 54: "stfd", 58: "ld", 62: "std"}


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("addrs", nargs="*")
    ap.add_argument("--string")
    a = ap.parse_args()
    data = open(a.image, "rb").read()
    targets = {int(x, 16) for x in a.addrs}
    if a.string:
        needle = a.string.encode() + b"\0"
        i = data.find(needle)
        while i != -1:
            if i == 0 or data[i - 1] == 0:
                targets.add(BASE + i)
                print(f"string '{a.string}' at {BASE + i:08X}")
            i = data.find(needle, i + 1)
    words = struct.unpack_from(f">{(TEXT[1] - TEXT[0]) // 4}I", data, TEXT[0] - BASE)
    for idx, w in enumerate(words):
        if (w >> 26) != 15 or ((w >> 16) & 31) != 0:
            continue
        rx = (w >> 21) & 31
        hi = (w & 0xFFFF) << 16
        for j in range(idx + 1, min(idx + 1 + WINDOW, len(words))):
            u = words[j]
            op = u >> 26
            ra = (u >> 16) & 31
            if op == 15 and ((u >> 21) & 31) == rx:
                break
            if op in OPS and ra == rx:
                lo = (u & 0xFFFF) if op == 24 else s16(u & 0xFFFF)
                if op == 58 or op == 62:
                    lo = s16(u & 0xFFFC)
                addr = (hi + lo) & 0xFFFFFFFF
                if addr in targets:
                    print(f"{TEXT[0] + 4 * idx:08X}: lis r{rx} ... {TEXT[0] + 4 * j:08X}: {OPS[op]} -> {addr:08X}")


if __name__ == "__main__":
    main()
