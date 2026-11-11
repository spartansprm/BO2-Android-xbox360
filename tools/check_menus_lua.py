import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KNOWN_GLOBALS = {"CoD", "Engine", "UIExpression", "_G", "error", "ipairs", "math", "pcall", "rawget", "rawset",
                 "string", "table", "tostring", "tonumber", "type", "pairs", "select", "unpack", "next",
                 "RegisterMaterial"}
MAX_TOP_LEVEL_NAMES = 92


def luac():
    for p in (shutil.which("luac5.1"), shutil.which("luac"), r"C:\msys64\ucrt64\bin\luac5.1.exe"):
        if p and os.path.isfile(p):
            return p
    sys.exit("no luac5.1: install it with MSYS2 (pacman -S mingw-w64-ucrt-x86_64-lua51)")


def top_level_names(text):
    count = 0
    for line in text.split("\n"):
        m = re.match(r"local function (\w+)", line) or re.match(r"local ([\w, ]+?)\s*(=|$)", line)
        if m:
            count += len(m.group(1).split(","))
    return count


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "app", "src", "ui", "menus.lua")
    compiler = luac()
    ok = True
    r = subprocess.run([compiler, "-p", path], capture_output=True, text=True)
    print("syntax: " + ("OK" if r.returncode == 0 else r.stderr.strip()))
    if r.returncode != 0:
        return 1
    listing = subprocess.run([compiler, "-l", "-p", path], capture_output=True, text=True).stdout
    written, read = {}, {}

    for line, kind, name in re.findall(r"\[(\d+)\]\s+(GET|SET)GLOBAL\s.*; (\w+)", listing):
        (written if kind == "SET" else read).setdefault(name, []).append(int(line))
    for name, lines in sorted(written.items()):
        ok = False
        print(f"WRITES the global {name} (lines {', '.join(map(str, sorted(set(lines))))})")
    for name, lines in sorted(read.items()):
        if name not in KNOWN_GLOBALS:
            ok = False
            print(f"reads an unknown global {name} (lines {', '.join(map(str, sorted(set(lines))))}): a local used "
                  "above its declaration, or a typo?")
    text = open(path, encoding="utf-8").read()
    names = top_level_names(text)
    lines = len(re.findall(r"\nlocal ", text))
    print(f"top level: {lines} local lines (the build allows 80), {names} names (at most {MAX_TOP_LEVEL_NAMES})")
    if lines > 80 or names > MAX_TOP_LEVEL_NAMES:
        ok = False
    print("all checks passed" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
