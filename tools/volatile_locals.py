#!/usr/bin/env python3


import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MARK = "// volatile_locals.py: the volatile registers are locals, written back to ctx at calls and returns"

SCALAR = r"r(?:1[0-3]|[0-9])|f(?:1[0-3]|[0-9])"
VECTOR = r"v(?:1[0-3]|[0-9]|3[2-9]|[45][0-9]|6[0-3])"
VOL = r"(?:" + SCALAR + "|" + VECTOR + ")"
VOL_NAMES = ([f"r{i}" for i in range(14)] + [f"f{i}" for i in range(14)] + [f"v{i}" for i in range(14)] +
             [f"v{i}" for i in range(32, 64)])
GBIT = {r: 1 << k for k, r in enumerate(VOL_NAMES)}
ALL = (1 << len(VOL_NAMES)) - 1
REF = re.compile(r"\bctx\.(" + VOL + r")\b")


WRITE = re.compile(r"\bctx\.(" + VOL + r")\b(?:\.\w+(?:\[[^\]]*\])?)?\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)")
VSTORE = re.compile(r"simde_mm_store\w*\(\s*(?:\([^)]*\)\s*)?ctx\.(" + VECTOR + r")\.\w+\s*,")


FULL_WRITE = re.compile(r"\bctx\.(" + VOL + r")\b(?:\.(?:u64|s64|f64))?\s*=(?!=)")
NON_OWNED = re.compile(r"\bctx\.(r(?:1[4-9]|2\d|3[01])|f(?:1[4-9]|2\d|3[01])|"
                       r"v(?:1[4-9]|2\d|3[01]|6[4-9]|[7-9]\d|1[01]\d|12[0-7])|cr[0-7]|ctr|xer)\b")
DECL = re.compile(r"^\tPPC(?:CR|V|XER)?Register (\w+)\{\};$", re.M)
CALL = re.compile(r"^\s*(?:[A-Za-z_]\w*\(ctx, base\);|REX_CALL_INDIRECT_FUNC\(.*\);(?:\s*\})?)\s*$")
DIRECT = re.compile(r"^\s*__imp__(sub_[0-9A-F]{8})\(ctx, base\);\s*$")
TRAP = re.compile(r"ppc_trap\(ctx, base, \d+\);")
RETURN = re.compile(r"\breturn;")
LABEL = re.compile(r"^(loc_[0-9A-F]+):$")
GOTO = re.compile(r"\bgoto (loc_[0-9A-F]+);")
FN = re.compile(r"^DEFINE_REX_FUNC\((sub_[0-9A-F]{8})\) \{\n(.*?)^\}\n", re.M | re.S)
PROLOGUE = "\tREX_FUNC_PROLOGUE();\n"
COMMENT = re.compile(r"//.*|/\*.*?\*/")


class Body:
    pass


    def __init__(self, body):
        self.lines = body[len(PROLOGUE):].split("\n") if body.startswith(PROLOGUE) else body.split("\n")
        lines = self.lines
        n = self.n = len(lines)
        labels = {}
        for i, line in enumerate(lines):
            m = LABEL.match(line)
            if m:
                labels[m.group(1)] = i
        self.kind = [None] * n
        self.callee = [None] * n
        self.tail_return = set()
        self.reads = [0] * n
        self.writes = [0] * n
        self.kills = [0] * n
        self.ends = [False] * n
        self.succ = [[] for _ in range(n)]
        for i, line in enumerate(lines):
            if CALL.match(line):
                j = i + 1
                while j < n and lines[j].strip().startswith("//"):
                    j += 1
                if j < n and lines[j].strip() == "return;":
                    self.kind[i] = "tail"
                    self.tail_return.add(j)
                else:
                    self.kind[i] = "call"
                m = DIRECT.match(line)
                if m:
                    self.callee[i] = m.group(1)
            elif line.strip() == "env = ctx;":
                self.kind[i] = "env_save"
            elif "ctx = env;" in line:
                self.kind[i] = "env_restore"
            elif TRAP.search(line):
                self.kind[i] = "trap"
            refs = REF.findall(line)
            if refs:
                vstores = VSTORE.findall(line)
                full = FULL_WRITE.findall(line) + vstores
                for r in WRITE.findall(line) + vstores:
                    self.writes[i] |= GBIT[r]
                for r in set(refs):
                    if refs.count(r) > full.count(r):
                        self.reads[i] |= GBIT[r]
                    elif r in full:
                        self.kills[i] |= GBIT[r]
            s = line.strip()
            top = line.startswith("\t") and not line.startswith("\t\t")
            self.ends[i] = top and (s.startswith("goto ") or s == "return;" or s.startswith("__builtin_trap"))
            if not self.ends[i] and i + 1 < n:
                self.succ[i].append(i + 1)
            for g in GOTO.findall(line):
                if g in labels:
                    self.succ[i].append(labels[g])


        opened = []
        for i, line in enumerate(lines):
            for ch in COMMENT.sub("", line):
                if ch == "{":
                    opened.append(i)
                elif ch == "}" and opened:
                    j = opened.pop()
                    if j != i:
                        self.succ[j].append(i)
        self.calls = {c for c in self.callee if c}

    def summary(self, summ):
        pass
        n = self.n
        out_mask = 0
        gen = [0] * n
        kill = [0] * n
        for i in range(n):
            k = self.kind[i]
            out_mask |= self.writes[i]
            if k in ("call", "tail"):
                c = self.callee[i]
                ci, co = summ.get(c, (ALL, ALL)) if c else (ALL, ALL)
                gen[i] = ci
                out_mask |= co
            elif k in ("env_save", "env_restore", "trap"):
                gen[i] = ALL
                out_mask = ALL
            else:
                gen[i] = self.reads[i]
                kill[i] = self.kills[i]

        pred = [[] for _ in range(n)]
        for i in range(n):
            for j in self.succ[i]:
                pred[j].append(i)
        live_in = [0] * n
        work = list(range(n))
        on = [True] * n
        while work:
            i = work.pop()
            on[i] = False
            lo = 0
            for j in self.succ[i]:
                lo |= live_in[j]
            if self.kind[i] == "tail":
                lo = 0
            li = gen[i] | (lo & ~kill[i])
            if li != live_in[i]:
                live_in[i] = li
                for p in pred[i]:
                    if not on[p]:
                        on[p] = True
                        work.append(p)
        return (live_in[0] if n else 0), out_mask


def summaries(bodies):
    pass

    summ = {name: (0, 0) for name in bodies}
    callers = {name: set() for name in bodies}
    for name, b in bodies.items():
        for c in b.calls:
            if c in callers:
                callers[c].add(name)
    work = list(bodies)
    on = set(work)
    while work:
        name = work.pop()
        on.discard(name)
        new = bodies[name].summary(summ)
        if new != summ[name]:
            summ[name] = new
            for c in callers[name]:
                if c not in on:
                    on.add(c)
                    work.append(c)
    return summ


def rewrite(text, b, summ, eligible=ALL):
    pass

    used_set = {r for r in REF.findall(text) if eligible & GBIT[r]}
    if not used_set or not text.startswith(PROLOGUE):
        return None
    declared = set(DECL.findall(text))
    if any(name not in declared for name in NON_OWNED.findall(text)):
        return None
    used = [r for r in VOL_NAMES if r in used_set]
    used_mask = 0
    for r in used:
        used_mask |= GBIT[r]
    lines, n, kind = b.lines, b.n, b.kind

    def callee_sets(i):
        pass
        c = b.callee[i]
        if summ is None or not c or c not in summ or kind[i] == "tail":
            return ALL, ALL
        ci, co = summ[c]
        return ci | co, co


    def out_of(i):
        k = kind[i]
        if k in ("call", "tail"):
            sync, _ = callee_sets(i)
            return dirty_in[i] & ~sync
        if k == "env_restore":
            return 0
        return dirty_in[i] | (b.writes[i] & used_mask)

    dirty_in = [0] * n
    reached = [False] * n
    work = []
    if n:
        reached[0] = True
        work.append(0)
    while work:
        i = work.pop()
        out = out_of(i)
        for j in b.succ[i]:
            if not reached[j] or (dirty_in[j] | out) != dirty_in[j]:
                reached[j] = True
                dirty_in[j] |= out
                work.append(j)


    wbm = [0] * n
    for i in range(n):
        if not reached[i]:
            continue
        k = kind[i]
        if k in ("call", "tail"):
            wbm[i] = dirty_in[i] & callee_sets(i)[0]
        elif k in ("env_save", "trap"):
            wbm[i] = dirty_in[i]
        elif i not in b.tail_return and RETURN.search(lines[i]):
            wbm[i] = dirty_in[i]
    last = max((i for i in range(n) if lines[i].strip()), default=-1)
    end_wb = 0
    if last >= 0 and reached[last] and not b.ends[last] and last not in b.tail_return:
        end_wb = out_of(last)


    pred = [[] for _ in range(n)]
    for i in range(n):
        for j in b.succ[i]:
            pred[j].append(i)
    live_in = [0] * n
    live_out = [0] * n
    work = list(range(n))
    on = [True] * n
    while work:
        i = work.pop()
        on[i] = False
        k = kind[i]
        lo = end_wb if i == last else 0
        if k != "tail":
            for j in b.succ[i]:
                lo |= live_in[j]
        live_out[i] = lo
        if k == "call":
            li = wbm[i] | (lo & ~callee_sets(i)[1])
        elif k == "tail":
            li = wbm[i]
        elif k == "env_restore":
            li = 0
        else:
            li = ((b.reads[i] | wbm[i]) & used_mask) | (lo & ~b.kills[i])
        if li != live_in[i]:
            live_in[i] = li
            for p in pred[i]:
                if not on[p]:
                    on[p] = True
                    work.append(p)

    def wb(mask):
        return " ".join(f"ctx.{r} = {r};" for r in used if mask & GBIT[r])

    def rl(mask):
        return " ".join(f"{r} = ctx.{r};" for r in used if mask & GBIT[r])

    def local(m):
        return m.group(1) if m.group(1) in used_set else m.group(0)

    out_lines = []
    for i, line in enumerate(lines):
        indent = line[:len(line) - len(line.lstrip("\t"))] or "\t"
        k = kind[i]
        w = wb(wbm[i])
        if k in ("call", "tail"):
            if w:
                out_lines.append(f"{indent}{w}")
            out_lines.append(line)
            r = rl(live_out[i] & callee_sets(i)[1]) if k == "call" and reached[i] else ""
            if r:
                out_lines.append(f"{indent}{r}")
        elif k == "env_save":
            if w:
                out_lines.append(f"{indent}{w}")
            out_lines.append(line)
        elif k == "env_restore":
            out_lines.append(REF.sub(local, line))
            r = rl(live_out[i])
            if r:
                out_lines.append(f"{indent}{r}")
        else:
            new = REF.sub(local, line)
            if k == "trap":
                new = TRAP.sub(lambda m: " ".join(x for x in ("{", w, m.group(0), rl(used_mask), "}") if x), new)
            elif w and i not in b.tail_return:
                new = RETURN.sub(f"{{ {w} return; }}", new)
            out_lines.append(new)
    while out_lines and out_lines[-1] == "":
        out_lines.pop()
    if end_wb:
        out_lines.append(f"\t{wb(end_wb)}")
    entry = live_in[0] if n else 0
    decls = "".join(f"\tPPC{'V' if r[0] == 'v' else ''}Register {r}" +
                    (f" = ctx.{r};\n" if entry & GBIT[r] else "{};\n") for r in used)
    return f"{PROLOGUE}\t{MARK}\n{decls}" + "\n".join(out_lines) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gen", default=os.path.join(ROOT, "app", "generated", "default_mp"))
    ap.add_argument("--list", help="only the functions named in this file (one sub_X per line, # comments)")
    ap.add_argument("--skip", default=os.path.join(ROOT, "app", "locals.txt"),
                    help="functions to leave as they are (tools/localize.py copies them from the original form)")
    ap.add_argument("--no-trim", action="store_true", help="write back and reload at every call as at an unknown one")
    ap.add_argument("--scalars-only", action="store_true", help="leave the vector registers in ctx")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()

    def names(path):
        out = set()
        if path and os.path.exists(path):
            for line in open(path, encoding="utf-8"):
                line = line.split("#", 1)[0].strip()
                if line:
                    out.add(line)
        return out

    only = names(a.list) if a.list else None
    skip = names(a.skip)
    files = sorted(f for f in os.listdir(a.gen) if re.fullmatch(r"bo2zm_recomp\.\d+\.cpp", f))
    texts = {f: open(os.path.join(a.gen, f), encoding="utf-8").read() for f in files}

    n_done = n_new = n_shared = n_skipped = n_none = 0
    pending = {}
    for f in files:
        for m in FN.finditer(texts[f]):
            name, body = m.group(1), m.group(2)
            if MARK in body:
                n_done += 1
            else:
                pending[name] = body
    if a.check:
        todo = [nm for nm, body in pending.items()
                if nm not in skip and (only is None or nm in only) and REF.search(body)
                and body.startswith(PROLOGUE)
                and not any(x not in set(DECL.findall(body)) for x in NON_OWNED.findall(body))]
        print(f"volatile_locals --check: {n_done} functions rewritten, {len(todo)} not")
        if todo:
            sys.exit(f"volatile_locals --check: {len(todo)} functions are not rewritten (first: {todo[0]}): run "
                     f"tools/codegen.sh (or python tools/volatile_locals.py on freshly generated code)")
        return
    if n_done and pending and not a.no_trim:
        sys.exit("volatile_locals: the code is partly rewritten already; the callee summaries need the original form. "
                 "Run it on freshly generated code (tools/codegen.sh)")

    eligible = ALL
    if a.scalars_only:
        eligible = 0
        for r in VOL_NAMES:
            if r[0] != "v":
                eligible |= GBIT[r]
    bodies = {name: Body(body) for name, body in pending.items()}
    summ = None if a.no_trim else summaries(bodies)
    if summ is not None:
        n_in = sum(bin(i).count("1") for i, _ in summ.values()) / max(1, len(summ))
        n_out = sum(bin(o).count("1") for _, o in summ.values()) / max(1, len(summ))
        print(f"volatile_locals: summaries of {len(summ)} functions, on average {n_in:.1f} registers read on entry "
              f"and {n_out:.1f} written (of {len(VOL_NAMES)})")

    for f in files:
        changed = False

        def one(m):
            nonlocal changed, n_new, n_shared, n_skipped, n_none
            name, body = m.group(1), m.group(2)
            if MARK in body:
                return m.group(0)
            if name in skip or (only is not None and name not in only):
                n_skipped += 1
                return m.group(0)
            new = rewrite(body, bodies[name], summ, eligible)
            if new is None:
                if REF.search(body):
                    n_shared += 1
                else:
                    n_none += 1
                return m.group(0)
            n_new += 1
            changed = True
            return f"DEFINE_REX_FUNC({name}) {{\n{new}}}\n"

        new_text = FN.sub(one, texts[f])
        if changed:
            open(os.path.join(a.gen, f), "w", encoding="utf-8", newline="\n").write(new_text)
    print(f"volatile_locals: {n_new} functions rewritten now, {n_done} already, {n_shared} left in ctx (shared "
          f"registers), {n_none} without volatile registers, {n_skipped} skipped (locals.txt or not listed)")


if __name__ == "__main__":
    main()
