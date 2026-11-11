import argparse
import re
import struct

ap = argparse.ArgumentParser()
ap.add_argument("zone")
ap.add_argument("--script", default="", help="only scripts whose name contains this")
ap.add_argument("--exports", action="store_true")
ap.add_argument("--imports", action="store_true")
ap.add_argument("--strings", action="store_true")
ap.add_argument("--grep", default="", help="only lines matching this regular expression (after the script line)")
ap.add_argument("--function", default="", help="the calls and strings of the exports with this name")
a = ap.parse_args()

data = open(a.zone, "rb").read()
starts = [m.start() for m in re.finditer(rb"\x80GSC\r\n\x00", data)]
pat = re.compile(a.grep, re.I) if a.grep else None


def u8(o):
    return data[o]


def u16(o):
    return struct.unpack_from(">H", data, o)[0]


def u32(o):
    return struct.unpack_from(">I", data, o)[0]


def text(o):
    end = data.find(b"\0", o)
    return data[o:end].decode("latin1") if end >= 0 and end - o < 512 else "?"


for i, h in enumerate(starts):
    try:
        name = text(h + u16(h + 0x30))
    except struct.error:
        continue
    if a.script and a.script not in name:
        continue
    lines = []
    includes = [text(h + u32(h + u32(h + 0x0C) + 4 * k)) for k in range(u8(h + 0x3C))]
    lines += [f"  include {s}" for s in includes]
    if a.exports:
        e = h + u32(h + 0x1C)
        for k in range(u16(h + 0x34)):
            crc, code, nm, params, flags = struct.unpack_from(">IIHBB", data, e + 12 * k)
            lines.append(f"  export {text(h + nm)}({params}) flags {flags:02X} code +{code:X}")
    if a.imports:
        o = h + u32(h + 0x20)
        for k in range(u16(h + 0x36)):
            nm, ns, uses, params, flags = struct.unpack_from(">HHHBB", data, o)
            space = text(h + ns) if ns else ""
            lines.append(f"  import {space + '::' if space else ''}{text(h + nm)}({params}) flags {flags:02X} x{uses}")
            o += 8 + 4 * uses
    if a.strings:
        o = h + u32(h + 0x18)
        for k in range(u16(h + 0x32)):
            s, uses, kind = struct.unpack_from(">HBB", data, o)
            lines.append(f"  {'field' if kind == 1 else 'string'} {text(h + s)!r} x{uses}")
            o += 4 + 4 * uses
    if a.function:
        e = h + u32(h + 0x1C)
        codes = sorted(struct.unpack_from(">I", data, e + 12 * k + 4)[0] for k in range(u16(h + 0x34)))
        uses = []
        o = h + u32(h + 0x20)
        for k in range(u16(h + 0x36)):
            nm, ns, count, params, flags = struct.unpack_from(">HHHBB", data, o)
            space = text(h + ns) if ns else ""
            call = f"call {space + '::' if space else ''}{text(h + nm)}({params}) flags {flags:02X}"
            uses += [(u32(o + 8 + 4 * j), call) for j in range(count)]
            o += 8 + 4 * count
        o = h + u32(h + 0x18)
        for k in range(u16(h + 0x32)):
            s, count, kind = struct.unpack_from(">HBB", data, o)
            item = f"{'field' if kind == 1 else 'string'} {text(h + s)!r}"
            uses += [(u32(o + 4 + 4 * j), item) for j in range(count)]
            o += 4 + 4 * count
        for k in range(u16(h + 0x34)):
            crc, code, nm, params, flags = struct.unpack_from(">IIHBB", data, e + 12 * k)
            if text(h + nm) != a.function:
                continue
            end = next((c for c in codes if c > code), 1 << 32)
            lines.append(f"  {a.function}({params}) code +{code:X}..+{end:X}")
            lines += [f"    +{at:X} {what}" for at, what in sorted(uses) if code <= at < end]
        if len(lines) == len(includes):
            continue
        lines = lines[len(includes):]
    if pat:
        lines = [l for l in lines if pat.search(l)]
        if not lines:
            continue
    print(f"{name}  (zone offset {h:X})")
    for l in lines:
        print(l)
