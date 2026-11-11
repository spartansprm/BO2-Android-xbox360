#!/usr/bin/env python3


import argparse
import os
import re
import sys

import numpy as np
from PIL import Image

CELL = 40


def find_burst(folder, burst):
    pass

    names = [n for n in os.listdir(folder) if re.match(r"(spot|location)_.+_\d{8}-\d{6}_\d{1,2}\.png$", n)]
    if not names:
        sys.exit(f"spot_flicker: no spot_*.png or location_*.png in {folder}")
    if burst is None:
        newest = max(names, key=lambda n: os.path.getmtime(os.path.join(folder, n)))
        burst = newest.rsplit("_", 1)[0]
    frames = sorted(n for n in names if n.rsplit("_", 1)[0] == burst)
    if len(frames) < 3:
        sys.exit(f"spot_flicker: {burst} has {len(frames)} screenshots; at least 3 are needed")
    return burst, [os.path.join(folder, n) for n in frames]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("--burst", help="the burst's name, without _NN.png (default: the newest)")
    ap.add_argument("--threshold", type=int, default=24, help="a change in any channel above this counts (0-255)")
    ap.add_argument("--out", help="write the map of what changed and blinked here (PNG)")
    ap.add_argument("--cells", type=int, default=12, help="how many of the busiest screen areas to list")
    a = ap.parse_args()

    burst, paths = find_burst(a.folder, a.burst)
    frames = np.stack([np.asarray(Image.open(p).convert("RGB"), dtype=np.int16) for p in paths])
    n, h, w, _ = frames.shape


    steps = np.abs(np.diff(frames, axis=0)).max(axis=3) > a.threshold
    jumps = steps.sum(axis=0)
    changed = jumps >= 1
    blinking = jumps >= 2
    total = h * w
    print(f"{burst}: {n} screenshots of {w}x{h}, threshold {a.threshold}")
    print(f"  changed:  {changed.sum():8d} pixels ({100.0 * changed.sum() / total:.3f} %)")
    print(f"  blinking: {blinking.sum():8d} pixels ({100.0 * blinking.sum() / total:.3f} %)")


    median = np.median(frames, axis=0)
    off = np.abs(frames - median).max(axis=3) > a.threshold
    shares = off.reshape(n, -1).mean(axis=1) * 100.0
    print("  pixels away from the burst's median, per screenshot (%): "
          + " ".join(f"{i + 1}:{s:.2f}" for i, s in enumerate(shares)))


    cells = []
    for y in range(0, h, CELL):
        for x in range(0, w, CELL):
            count = int(blinking[y:y + CELL, x:x + CELL].sum())
            if count:
                cells.append((count, x, y))
    cells.sort(reverse=True)
    if cells:
        print(f"  areas with the most blinking pixels (x, y of the {CELL}x{CELL} cell's top left):")
        for count, x, y in cells[:a.cells]:
            print(f"    {x:4d},{y:3d}  {count:5d} pixels ({100.0 * count / (CELL * CELL):.0f} % of the cell)")
    else:
        print("  no blinking area")

    if a.out:
        picture = (frames[0] * 0.35).astype(np.uint8)
        picture[changed] = (255, 220, 0)
        picture[blinking] = (255, 0, 0)
        Image.fromarray(picture).save(a.out)
        print(f"  map: {a.out} (red blinking, yellow changed)")


if __name__ == "__main__":
    main()
