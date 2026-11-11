#!/usr/bin/env python3


import re
import struct
import sys

OPCODES = [
    "GETFIELD", "TEST", "CALL_I", "CALL_C", "EQ", "EQ_BK", "GETGLOBAL", "MOVE", "SELF", "RETURN", "GETTABLE_S",
    "GETTABLE_N", "GETTABLE", "LOADBOOL", "TFORLOOP", "SETFIELD", "SETTABLE_S", "SETTABLE_S_BK", "SETTABLE_N",
    "SETTABLE_N_BK", "SETTABLE", "SETTABLE_BK", "TAILCALL_I", "TAILCALL_C", "TAILCALL_M", "LOADK", "LOADNIL",
    "SETGLOBAL", "JMP", "CALL_M", "CALL", "INTRINSIC_INDEX", "INTRINSIC_NEWINDEX", "INTRINSIC_SELF",
    "INTRINSIC_INDEX_LITERAL", "INTRINSIC_NEWINDEX_LITERAL", "INTRINSIC_SELF_LITERAL", "TAILCALL", "GETUPVAL",
    "SETUPVAL", "ADD", "ADD_BK", "SUB", "SUB_BK", "MUL", "MUL_BK", "DIV", "DIV_BK", "MOD", "MOD_BK", "POW", "POW_BK",
    "NEWTABLE", "UNM", "NOT", "LEN", "LT", "LT_BK", "LE", "LE_BK", "CONCAT", "TESTSET", "FORPREP", "FORLOOP",
    "SETLIST", "CLOSE", "CLOSURE", "VARARG", "TAILCALL_I_R1", "CALL_I_R1", "SETUPVAL_R1", "TEST_R1", "NOT_R1",
    "GETFIELD_R1", "SETFIELD_R1", "NEWSTRUCT", "DATA", "SETSLOTN", "SETSLOTI", "SETSLOT", "SETSLOTS", "SETSLOTMT",
    "CHECKTYPE", "CHECKTYPES", "GETSLOT", "GETSLOTMT", "SELFSLOT", "SELFSLOTMT", "GETFIELD_MM", "CHECKTYPE_D",
    "GETSLOT_D", "GETGLOBAL_MEM",
]
ARITH = {"ADD": "+", "SUB": "-", "MUL": "*", "DIV": "/", "MOD": "%", "POW": "^"}


class Function:
    pass


class Reader:
    def __init__(self, data):
        self.d = data
        self.p = 0

    def u8(self):
        v = self.d[self.p]
        self.p += 1
        return v

    def u32(self):
        v = struct.unpack_from(">I", self.d, self.p)[0]
        self.p += 4
        return v

    def f32(self):
        v = struct.unpack_from(">f", self.d, self.p)[0]
        self.p += 4
        return v

    def function(self):
        f = Function()
        f.upvalues = self.u32()
        f.params = self.u32()
        f.vararg = self.u8()
        f.max_stack = self.u32()
        count = self.u32()
        self.p = (self.p + 3) & ~3
        f.code = [self.u32() for _ in range(count)]
        f.constants = []
        for _ in range(self.u32()):
            kind = self.u8()
            if kind == 0:
                f.constants.append(None)
            elif kind == 1:
                f.constants.append(bool(self.u8()))
            elif kind == 3:
                f.constants.append(self.f32())
            elif kind == 4:
                n = self.u32()
                f.constants.append(self.d[self.p:self.p + n - 1].decode("latin1"))
                self.p += n
            else:
                raise ValueError("constant type %d at %#x" % (kind, self.p - 1))
        f.debug = self.u32()
        f.hash = self.u32() if f.debug else None
        f.functions = [self.function() for _ in range(self.u32())]
        return f


def parse_chunk(data):
    r = Reader(data)
    if data[:4] != b"\x1bLua" or data[4] != 0x51 or data[5] != 0x0D or data[6] != 0:
        raise ValueError("not a big-endian HavokScript 5.1 chunk")
    r.p = 14
    for _ in range(r.u32()):
        r.u32()
        length = r.u32()
        r.p += length
    main = r.function()
    return main, r.p


def find_chunks(zone):
    pass
    for m in re.finditer(rb"([A-Za-z0-9_/.]+\.lua)\x00\x1bLuaQ", zone):
        start = m.start(1)
        if zone[start - 4:start] != b"\xff\xff\xff\xff":
            continue
        length = struct.unpack_from(">I", zone, start - 8)[0]
        yield m.group(1).decode(), m.end(1) + 1, length


def k(f, i):
    v = f.constants[i] if i < len(f.constants) else "<K%d?>" % i
    if isinstance(v, str):
        return '"%s"' % v
    if v is None:
        return "nil"
    if isinstance(v, bool):
        return "true" if v else "false"
    return ("%g" % v)


def name_of(f, i):
    v = f.constants[i] if i < len(f.constants) else None
    return v if isinstance(v, str) and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", v) else None


def field(base, f, i):
    n = name_of(f, i)
    return "%s.%s" % (base, n) if n else "%s[%s]" % (base, k(f, i))


def disassemble(f, path, out, raw):
    out.append("-- function %s: %d params%s, %d upvalues, %d instructions" %
               (path, f.params, " + ..." if f.vararg & 2 or f.vararg & 1 else "", f.upvalues, len(f.code)))
    reg = {}

    def R(i):
        return reg.get(i, "r%d" % i)

    def RK(c):
        return k(f, c & 0xFF) if c & 0x100 else R(c)

    for i in range(f.params):
        reg[i] = "p%d" % i
    multi = None
    pc = 0
    while pc < len(f.code):
        w = f.code[pc]
        op = w >> 25
        a, c, b = w & 0xFF, (w >> 8) & 0x1FF, (w >> 17) & 0xFF
        bx = (w >> 8) & 0x1FFFF
        name = OPCODES[op] if op < len(OPCODES) else "OP%d" % op
        text = None
        if name == "GETGLOBAL":
            reg[a] = f.constants[bx] if bx < len(f.constants) else "<K%d?>" % bx
        elif name == "LOADK":
            reg[a] = k(f, bx)
        elif name == "LOADBOOL":
            reg[a] = "true" if b else "false"
        elif name == "LOADNIL":
            for r in range(a, b + 1):
                reg[r] = "nil"
        elif name == "MOVE":
            text = "r%d = %s" % (a, R(b))

            reg[a] = "r%d" % a if re.fullmatch(r"r\d+", R(b)) else R(b)
        elif name == "GETUPVAL":
            reg[a] = "u%d" % b
        elif name in ("GETFIELD", "GETFIELD_R1", "GETFIELD_MM"):
            reg[a] = field(R(b), f, c & 0xFF)
        elif name in ("GETTABLE", "GETTABLE_S", "GETTABLE_N"):
            reg[a] = "%s[%s]" % (R(b), RK(c))
        elif name == "SELF":
            obj = R(b)
            reg[a + 1] = obj
            reg[a] = "%s:%s" % (obj, name_of(f, c & 0xFF) or k(f, c & 0xFF)) if c & 0x100 or name_of(f, c) else \
                "%s:[%s]" % (obj, RK(c))
        elif name in ("SETFIELD", "SETFIELD_R1"):
            text = "%s = %s" % (field(R(a), f, b), RK(c))
        elif name in ("SETTABLE", "SETTABLE_S", "SETTABLE_N"):
            text = "%s[%s] = %s" % (R(a), R(b), RK(c))
        elif name in ("SETTABLE_BK", "SETTABLE_S_BK", "SETTABLE_N_BK"):
            text = "%s[%s] = %s" % (R(a), k(f, b), RK(c))
        elif name == "SETGLOBAL":
            text = "%s = %s" % (f.constants[bx], R(a))
        elif name in ("SETUPVAL", "SETUPVAL_R1"):
            text = "u%d = %s" % (b, R(a))
        elif name in ("CALL", "CALL_I", "CALL_C", "CALL_M", "CALL_I_R1", "TAILCALL", "TAILCALL_I", "TAILCALL_C",
                      "TAILCALL_M", "TAILCALL_I_R1"):
            fn = R(a)
            method = ":" in fn and reg.get(a + 1) is not None and fn.startswith(reg.get(a + 1) + ":")
            first = a + 2 if method else a + 1
            if b:
                args = [R(r) for r in range(first, a + b)]
            elif multi:
                args = [R(r) for r in range(first, multi[0])] + [multi[1]]
            else:
                args = ["..."]
            multi = None
            call = "%s(%s)" % (fn, ", ".join(args))
            tail = name.startswith("TAILCALL")
            if c == 0 and not tail:
                multi = (a, call)
                pc += 1
                continue
            if tail:
                text = "return " + call
            elif c == 1:
                text = call
            elif c == 2:
                text = "r%d = %s" % (a, call)
                reg[a] = "r%d" % a
            else:
                text = "%s = %s" % (", ".join("r%d" % r for r in range(a, a + c - 1)) if c else "r%d..." % a, call)
                for r in range(a, a + max(c - 1, 1)):
                    reg[r] = "r%d" % r
        elif name == "RETURN":
            if b > 1:
                text = "return " + ", ".join(R(r) for r in range(a, a + b - 1))
            elif b == 1:
                text = "return"
            else:
                text = "return " + ", ".join([R(r) for r in range(a, multi[0])] + [multi[1]] if multi else ["..."])
                multi = None
        elif name == "CLOSURE":
            reg[a] = "function_%s.%d" % (path, bx)
            text = "r%d = function %s.%d" % (a, path, bx)
        elif name == "NEWTABLE":
            reg[a] = "r%d" % a
            text = "r%d = {}" % a
        elif name.rstrip("_BK") in ARITH or name in ARITH:
            base = name[:-3] if name.endswith("_BK") else name
            left = k(f, b) if name.endswith("_BK") else R(b)
            reg[a] = "(%s %s %s)" % (left, ARITH[base], RK(c))
        elif name == "CONCAT":
            reg[a] = "(" + " .. ".join(R(r) for r in range(b, c + 1)) + ")"
        elif name in ("NOT", "NOT_R1"):
            reg[a] = "not %s" % R(b)
        elif name == "UNM":
            reg[a] = "-%s" % R(b)
        elif name == "LEN":
            reg[a] = "#%s" % R(b)
        elif name == "JMP":
            text = "goto %d" % (pc + 1 + bx - 0xFFFF)
        elif name in ("TEST", "TEST_R1"):
            text = "if %s%s then skip next" % ("" if c else "not ", R(a))
        elif name in ("EQ", "EQ_BK", "LT", "LT_BK", "LE", "LE_BK"):
            base = name[:2]
            left = k(f, b) if name.endswith("_BK") else R(b)
            sym = {"EQ": "==", "LT": "<", "LE": "<="}[base]
            text = "if (%s %s %s) %s %s then skip next" % (left, sym, RK(c), "~=" if a == 0 else "==", "true")
        elif name == "DATA":
            if not raw:
                pc += 1
                continue
        elif name in ("FORPREP", "FORLOOP"):
            text = "%s r%d goto %d" % (name.lower(), a, pc + 1 + bx - 0xFFFF)
            reg[a + 3] = "r%d" % (a + 3)
        elif name == "TFORLOOP":
            text = "tforloop r%d (%d results)" % (a, c)
            for r in range(a + 3, a + 3 + c):
                reg[r] = "r%d" % r
        if raw:
            text = (text + "    " if text else "") + "[%s A=%d B=%d C=%d Bx=%d]" % (name, a, b, c, bx)
        elif text is None and name not in ("GETGLOBAL", "LOADK", "LOADBOOL", "LOADNIL", "GETUPVAL", "GETFIELD",
                                            "GETFIELD_R1", "GETFIELD_MM", "GETTABLE", "GETTABLE_S", "GETTABLE_N",
                                            "SELF", "CONCAT", "NOT", "NOT_R1", "UNM", "LEN") and                 name not in ARITH and name.replace("_BK", "") not in ARITH:
            text = "[%s A=%d B=%d C=%d Bx=%d]" % (name, a, b, c, bx)
        if text is not None:
            out.append("  %3d  %s" % (pc, text))
        pc += 1
    for i, sub in enumerate(f.functions):
        out.append("")
        disassemble(sub, "%s.%d" % (path, i), out, raw)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    zone = open(sys.argv[1], "rb").read()
    if sys.argv[2] == "--list":
        for name, offset, length in find_chunks(zone):
            print("%-60s %#10x %7d" % (name, offset, length))
        return 0
    raw = "--raw" in sys.argv
    for name, offset, length in find_chunks(zone):
        if name != sys.argv[2]:
            continue
        main_function, end = parse_chunk(zone[offset:offset + length])
        if end != length:
            print("-- warning: parsed %d of %d bytes" % (end, length))
        out = []
        disassemble(main_function, "f", out, raw)
        print("\n".join(out))
        return 0
    print("not found: " + sys.argv[2])
    return 1


if __name__ == "__main__":
    sys.exit(main())
