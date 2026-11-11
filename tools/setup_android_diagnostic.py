#!/usr/bin/env python3

from pathlib import Path
import subprocess
import sys

SDL_COMMIT = 'c9a6709bd21750f1ad9597be21abace78c6378c9'
root = Path(__file__).resolve().parents[1]
target = root / 'android' / 'sdl3'
stamp = target / '.bo2-diagnostic-revision'
if stamp.exists() and stamp.read_text().strip() == SDL_COMMIT:
    print('SDL3 pinned source already present')
    sys.exit(0)
if target.exists() and any(target.iterdir()):
    sys.exit(f'{target} is not an empty managed checkout; move it aside first')
target.mkdir(parents=True, exist_ok=True)
subprocess.run(['git', 'init', '-q', str(target)], check=True)
subprocess.run(['git', '-C', str(target), 'remote', 'add', 'origin',
                'https://github.com/libsdl-org/SDL.git'], check=True)
subprocess.run(['git', '-C', str(target), 'fetch', '--depth=1', 'origin', SDL_COMMIT], check=True)
subprocess.run(['git', '-C', str(target), 'checkout', '--detach', 'FETCH_HEAD'], check=True)
stamp.write_text(SDL_COMMIT + '\n')
print(f'SDL3 ready at {SDL_COMMIT}')
