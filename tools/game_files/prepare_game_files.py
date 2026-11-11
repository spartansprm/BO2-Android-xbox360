#!/usr/bin/env python3


import argparse
import hashlib
import os
import shutil
import subprocess
import sys

TU18_XEXP_SHA256 = "5d5780970f6e639b17c0570bae0e52a13bd1038e23c969428aef9d0c7d78681b"


TU18_XEXP_SHA256_VARIANTS = frozenset((
    TU18_XEXP_SHA256,
    "7f4ee5e4b5ccfe71786ddc073a0363558e89634bb26c2929335aeb7f883c9499",
))
TU_NAME = "tu00000002_00000000"
HERE = os.path.dirname(os.path.abspath(__file__))


def say(text=""):
    print(text, flush=True)


def fail(text):
    say(f"\nStopped: {text}")
    sys.exit(1)


def helper(name):
    pass
    for path in (os.path.join(HERE, "lib", name), os.path.join(HERE, "lib", "inspect", name),
                 os.path.join(HERE, "..", name), os.path.join(HERE, "..", "inspect", name)):
        if os.path.isfile(path):
            return os.path.abspath(path)
    fail(f"{name} is missing: keep the lib folder next to this script")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def is_disc(folder):
    return os.path.isfile(os.path.join(folder, "default_mp.xex"))


def find_disc():
    pass
    places = [HERE, os.getcwd()]
    for base in (HERE, os.getcwd()):
        try:
            places += [os.path.join(base, d) for d in sorted(os.listdir(base)) if os.path.isdir(os.path.join(base, d))]
        except OSError:
            pass
    found = []
    for p in places:
        p = os.path.abspath(p)
        if p not in found and is_disc(p):
            found.append(p)
    if len(found) == 1:
        return found[0]
    if not found:
        fail("no disc folder found. Put your disc dump folder next to this script, or pass it: "
             "python prepare_game_files.py \"<your disc folder>\"")
    fail("more than one disc folder found (" + ", ".join(found) + "): pass the one to use")


def find_title_update(disc):
    usual = os.path.join(disc, "DLC", "415608C3", "000B0000", TU_NAME)
    if os.path.isfile(usual):
        return usual
    for root, _dirs, files in os.walk(disc):
        if TU_NAME in files:
            return os.path.join(root, TU_NAME)
    for p in (os.path.join(HERE, TU_NAME), os.path.join(os.getcwd(), TU_NAME)):
        if os.path.isfile(p):
            return p
    fail(f"Title Update 18 ({TU_NAME}) not found: pass it with --title-update <file>")


def folder_size(folder, skip_top=None):
    total = 0
    for root, dirs, files in os.walk(folder):
        if skip_top and os.path.abspath(root) == os.path.abspath(folder):
            dirs[:] = [d for d in dirs if d != skip_top]
        total += sum(os.path.getsize(os.path.join(root, f)) for f in files)
    return total


def copy_tree(src, dst, skip_top=None):
    pass
    copied = kept = 0
    for root, dirs, files in os.walk(src):
        if skip_top and os.path.abspath(root) == os.path.abspath(src):
            dirs[:] = [d for d in dirs if d != skip_top]
        rel = os.path.relpath(root, src)
        out = os.path.join(dst, rel) if rel != "." else dst
        os.makedirs(out, exist_ok=True)
        for f in files:
            s, d = os.path.join(root, f), os.path.join(out, f)
            if os.path.isfile(d) and os.path.getsize(d) == os.path.getsize(s):
                kept += 1
                continue
            shutil.copy2(s, d + ".part")
            os.replace(d + ".part", d)
            copied += 1
            if copied % 50 == 0:
                say(f"    {copied} files copied...")
    return copied, kept


def run(script, *args):
    r = subprocess.run([sys.executable, "-I", script, *args])
    if r.returncode:
        fail(f"{os.path.basename(script)} failed (exit {r.returncode})")


def main():
    ap = argparse.ArgumentParser(description="Lay out your Black Ops II game files for bo2-zm-nx (switch/bo2zm).")
    ap.add_argument("disc", nargs="?", help="the extracted disc folder (the one with default_mp.xex)")
    ap.add_argument("--out", default=os.path.join(HERE, "output"),
                    help="where to write switch/bo2zm/ (default: output/ next to this script)")
    ap.add_argument("--title-update", help=f"the Title Update 18 package ({TU_NAME}), if it is not in the dump")
    ap.add_argument("--dlc", help="the folder with your DLC packages, if they are not in the dump")
    ap.add_argument("--no-dlc", action="store_true", help="leave the DLC out")
    ap.add_argument("--all-dlc-files", action="store_true",
                    help="keep every DLC file (by default other languages and multiplayer-only files are left out)")
    a = ap.parse_args()

    disc = os.path.abspath(a.disc) if a.disc else find_disc()
    if not is_disc(disc):
        fail(f"{disc} has no default_mp.xex: point to the extracted disc folder")
    tu = os.path.abspath(a.title_update) if a.title_update else find_title_update(disc)
    if not os.path.isfile(tu):
        fail(f"title update not found: {tu}")
    dlc = None
    if not a.no_dlc:
        dlc = os.path.abspath(a.dlc) if a.dlc else os.path.join(disc, "DLC", "415608C3", "00000002")
        if not os.path.isdir(dlc):
            if a.dlc:
                fail(f"DLC folder not found: {dlc}")
            dlc = None
    out = os.path.abspath(a.out)
    bo2zm = os.path.join(out, "switch", "bo2zm")

    say("bo2-zm-nx game files")
    say(f"  disc folder:   {disc}")
    say(f"  title update:  {tu}")
    say(f"  DLC:           {dlc or 'none'}")
    say(f"  output:        {bo2zm}")

    need = folder_size(disc, skip_top="DLC") + os.path.getsize(tu) * 2 + (folder_size(dlc) if dlc else 0)
    os.makedirs(out, exist_ok=True)
    free = shutil.disk_usage(out).free
    if free < need:
        fail(f"not enough free space at the output: about {need / 2**30:.1f} GB needed, {free / 2**30:.1f} GB free")

    say("\n1/3  The disc's files -> game/")
    copied, kept = copy_tree(disc, os.path.join(bo2zm, "game"), skip_top="DLC")
    say(f"     {copied} copied, {kept} already there")

    say("2/3  Title Update 18 -> update/")
    update = os.path.join(bo2zm, "update")
    run(helper("stfs_extract.py"), tu, "--out", update)
    xexp = os.path.join(update, "default_mp.xexp")
    if not os.path.isfile(xexp) or sha256(xexp) not in TU18_XEXP_SHA256_VARIANTS:
        fail("this title update is not TU18 (its default_mp.xexp does not match): bo2-zm-nx needs Title Update 18, "
             f"the package named {TU_NAME}")
    for name in ("default.xexp", "default_mp.xexp"):
        if os.path.isfile(os.path.join(update, name)):
            shutil.copy2(os.path.join(update, name), os.path.join(bo2zm, "game", name))
    say("     TU18 checked and extracted; default.xexp and default_mp.xexp copied into game/")

    if dlc:
        say("3/3  DLC -> user/0000000000000000/415608C3/")
        run(helper("install_dlc.py"), dlc, "--out", os.path.join(bo2zm, "user"),
            *(["--all"] if a.all_dlc_files else []))
    else:
        say("3/3  DLC: none")

    say(f"\nDone. Merge the switch folder from {out} onto the root of your SD card,")
    say("next to (or into) the switch/bo2zm folder that holds bo2zm.nro and the shader library.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        say("\nStopped. Run the same command again to carry on.")
        sys.exit(130)
