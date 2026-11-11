#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only

import argparse
import json
from pathlib import Path
import re


def check(directory):
    counts = dict.fromkeys(('sync', 'lwsync', 'eieio', 'isync'), 0)
    files = sorted(directory.glob('bo2zm_recomp.*.cpp'))
    if not files:
        raise ValueError('Private generated C++ is missing; run the modified host codegen first')
    for file in files:
        lines = file.read_text().splitlines()
        for index, line in enumerate(lines):
            match = re.match(r'\s*// (sync|lwsync|eieio|isync)(?:\s|$)', line)
            if match:
                following = '\n'.join(lines[index+1:index+6])
                if 'std::atomic_thread_fence(std::memory_order_seq_cst)' not in following:
                    raise ValueError(f'Missing Android fence at {file.name}:{index+1}; regenerate code')
                counts[match[1]] += 1
    if not sum(counts.values()):
        raise ValueError('No guest ordering instructions found')
    return {'translation_units': len(files), 'fences': counts, 'missing': 0}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--generated', type=Path, default=Path(__file__).resolve().parents[1] / 'app/generated/default_mp')
    args = parser.parse_args()
    print(json.dumps(check(args.generated), indent=2))


if __name__ == '__main__':
    main()
