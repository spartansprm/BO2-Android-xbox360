import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "app", "generated", "default_mp")
BARRIER = "\tREX_GUEST_BARRIER();"
LABEL = re.compile(r"^loc_[0-9A-F]+:\s*$")
ORDERING = re.compile(r"^\s*// (sync|lwsync|eieio|isync|db16cyc)\b")


def recomp_files(gen):
    return sorted(os.path.join(gen, f) for f in os.listdir(gen)
                  if re.fullmatch(r"\w+_recomp\.\d+\.cpp", f))


def rewrite(path, undo):
    lines = open(path, encoding="utf-8").read().split("\n")
    out = []
    labels = orderings = 0
    i = 0
    while i < len(lines):
        line = lines[i]
        if line == BARRIER:
            i += 1
            continue
        out.append(line)
        if not undo:
            if LABEL.match(line):
                out.append(BARRIER)
                labels += 1
            elif ORDERING.match(line):
                out.append(BARRIER)
                orderings += 1
        i += 1
    return "\n".join(out), labels, orderings


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gen", default=GEN)
    ap.add_argument("--undo", action="store_true", help="remove every barrier line")
    ap.add_argument("--check", action="store_true", help="change nothing; fail if a file is not as this tool leaves it")
    a = ap.parse_args()
    labels = orderings = changed = 0
    stale = []
    for path in recomp_files(a.gen):
        text = open(path, encoding="utf-8").read()
        new, n_labels, n_orderings = rewrite(path, a.undo)
        labels += n_labels
        orderings += n_orderings
        if new != text:
            if a.check:
                stale.append(os.path.basename(path))
            else:
                open(path, "w", encoding="utf-8", newline="\n").write(new)
                changed += 1
    if a.check:
        if stale:
            print(f"guest_barriers --check: {len(stale)} files without their barriers (first: {stale[0]}); "
                  f"fix: python tools/guest_barriers.py --gen {a.gen}", file=sys.stderr)
            sys.exit(1)
        print(f"guest_barriers --check: ok ({labels} labels, {orderings} ordering instructions)")
        return
    verb = "removed" if a.undo else "placed"
    print(f"guest_barriers: {verb}: {labels} after labels, {orderings} after ordering instructions; "
          f"{changed} files changed")


if __name__ == "__main__":
    main()
