#!/usr/bin/env python3


import os
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
TOP = "bo2-zm-nx game files"
FILES = [
    (os.path.join(HERE, "prepare_game_files.py"), "prepare_game_files.py"),
    (os.path.join(HERE, "README.txt"), "README.txt"),
    (os.path.join(ROOT, "LICENSE"), "LICENSE"),
    (os.path.join(ROOT, "tools", "install_dlc.py"), "lib/install_dlc.py"),
    (os.path.join(ROOT, "tools", "inspect", "stfs_extract.py"), "lib/inspect/stfs_extract.py"),
]

out = os.path.abspath(sys.argv[1])
os.makedirs(os.path.dirname(out), exist_ok=True)
with zipfile.ZipFile(out + ".part", "w", zipfile.ZIP_DEFLATED) as z:
    for src, name in FILES:
        z.write(src, f"{TOP}/{name}")
os.replace(out + ".part", out)
print(f"{out}: {os.path.getsize(out):,} bytes")
