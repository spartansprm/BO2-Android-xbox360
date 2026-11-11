import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "app", "generated", "default_mp")
SOURCES = [os.path.join(ROOT, "app", "src"), os.path.join(ROOT, "app", "CMakeLists.txt"),
           os.path.join(ROOT, "android", "game"),
           os.path.join(ROOT, "sdk", "src"), os.path.join(ROOT, "sdk", "include")]


CAMPAIGN_SOURCES = [os.path.join(ROOT, "campaign", "src"), os.path.join(ROOT, "campaign", "CMakeLists.txt")]
SKIP_EXT = (".a", ".o", ".obj", ".png", ".jpg", ".bin", ".lib", ".dll", ".exe", ".pdb", ".spv")
CALL_ONLY = "call only"
ADDRESS = re.compile(r"(?<![0-9A-Fa-f])(82[0-9A-Fa-f]{6})(?![0-9A-Fa-f])")
CALL = re.compile(r"(?<![\w])(__imp__)?sub_([0-9A-F]{8})\(ctx, base\);")


def hooked(gen=GEN):
    names = set()
    sources = SOURCES + (CAMPAIGN_SOURCES if "campaign" in os.path.normpath(gen).split(os.sep) else [])
    for root in sources:
        if os.path.isfile(root):
            paths = [root]
        else:
            paths = [os.path.join(d, f) for d, _, fs in os.walk(root) for f in fs if not f.endswith(SKIP_EXT)]
        for path in paths:
            try:
                text = open(path, encoding="utf-8", errors="ignore").read()
            except OSError:
                continue
            for line in text.splitlines():
                if CALL_ONLY in line:
                    continue
                names.update("sub_" + m.upper() for m in ADDRESS.findall(line.split("//", 1)[0]))
    return names


def recomp_files(gen):
    return sorted(os.path.join(gen, f) for f in os.listdir(gen)
                  if re.fullmatch(r"\w+_recomp\.\d+\.cpp", f))


def rewrite(gen, with_hook, undo, check_only):
    direct = kept = changed_calls = files = 0
    bypassed = {}
    for path in recomp_files(gen):
        text = open(path, encoding="utf-8").read()
        counts = [0, 0, 0]

        def fix(m):
            name = "sub_" + m.group(2)
            want_direct = not undo and name not in with_hook
            is_direct = m.group(1) is not None
            if is_direct and name in with_hook:
                bypassed[name] = bypassed.get(name, 0) + 1
            counts[0 if want_direct else 1] += 1
            if want_direct == is_direct:
                return m.group(0)
            counts[2] += 1
            return ("__imp__" if want_direct else "") + name + "(ctx, base);"

        new = CALL.sub(fix, text)
        direct += counts[0]
        kept += counts[1]
        changed_calls += counts[2]
        if new != text and not check_only:
            open(path, "w", encoding="utf-8", newline="\n").write(new)
            files += 1
    return direct, kept, changed_calls, files, bypassed


def defined_hooks(build_dir, nm):
    pass
    found = {}
    for d, _, fs in os.walk(build_dir):
        if "generated" in d.replace("\\", "/").split("/") or "sdk-out" in d:
            continue
        for f in fs:
            if not f.endswith((".o", ".obj")):
                continue
            path = os.path.join(d, f)
            out = subprocess.run([nm, "--defined-only", path], capture_output=True, text=True).stdout
            for line in out.splitlines():
                parts = line.split()
                if len(parts) >= 3 and parts[1] == "T" and re.fullmatch(r"sub_[0-9A-F]{8}", parts[2]):
                    found.setdefault(parts[2], path)
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gen", default=GEN)
    ap.add_argument("--undo", action="store_true", help="every call back to sub_X")
    ap.add_argument("--check", action="store_true", help="change nothing; fail if a hooked function is called directly")
    ap.add_argument("--objects", help="build folder whose app objects are checked against the hooked list")
    ap.add_argument("--nm", default="llvm-nm")
    a = ap.parse_args()
    with_hook = hooked(a.gen)
    direct, kept, changed, files, bypassed = rewrite(a.gen, with_hook, a.undo, a.check)
    if a.check:
        if bypassed:
            print("direct_calls: these hooked functions are called directly (__imp__sub_X), so their hooks only "
                  "see indirect calls:", file=sys.stderr)
            for name, n in sorted(bypassed.items()):
                print(f"  {name}: {n} calls", file=sys.stderr)
            print(f"  fix: python tools/direct_calls.py --gen {a.gen}", file=sys.stderr)
            sys.exit(1)
        print(f"direct_calls --check: ok ({direct} direct calls, {kept} through sub_X, "
              f"{len(with_hook)} hooked addresses)")
    else:
        print(f"direct_calls: {'undone' if a.undo else 'done'}: {direct} direct calls, {kept} through sub_X; "
              f"{changed} calls changed in {files} files; {len(with_hook)} hooked addresses respected")
    if a.objects:
        defined = defined_hooks(a.objects, a.nm)
        missing = {n: p for n, p in defined.items() if n not in with_hook}
        print(f"direct_calls --objects: {len(defined)} hooks defined in {a.objects}; "
              f"{len(missing)} missing from the hooked list")
        for n, p in sorted(missing.items()):
            print(f"  {n} defined in {p}", file=sys.stderr)
        if missing:
            sys.exit(1)


if __name__ == "__main__":
    main()
