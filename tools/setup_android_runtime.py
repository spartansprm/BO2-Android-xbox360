#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only


import argparse
import hashlib
import io
import json
import os
import shutil
from pathlib import Path, PurePosixPath
import tarfile
from urllib.request import urlopen


def obtain(item, cache, target, sdk=False):
    repo, commit = item['repo'], item['commit']
    archive = cache / (repo.split('/')[-1] + '-' + commit + '.tar.gz')
    if not archive.exists():
        with urlopen(f'https://codeload.github.com/{repo}/tar.gz/{commit}', timeout=60) as response:
            archive.write_bytes(response.read())
    data = archive.read_bytes()
    if hashlib.sha256(data).hexdigest() != item['archive_sha256']:
        raise ValueError(f'Runtime dependency checksum mismatch: {repo}')
    deferred_windows_links = []
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as package:
        for entry in package.getmembers():
            path = PurePosixPath(entry.name)
            if path.is_absolute() or '..' in path.parts:
                raise ValueError('Unsafe dependency archive path')
            parts = path.parts[1:]
            if sdk:
                if not parts or parts[0] != 'thirdparty':
                    continue
                parts = parts[1:]
            if not parts or '.git' in parts:
                continue
            dest = target.joinpath(*parts)
            if dest.exists() or dest.is_symlink():
                continue
            if entry.isdir():
                dest.mkdir(parents=True, exist_ok=True)
            elif entry.isfile():
                dest.parent.mkdir(parents=True, exist_ok=True)
                with package.extractfile(entry) as source:
                    dest.write_bytes(source.read())
            elif entry.issym():
                dest.parent.mkdir(parents=True, exist_ok=True)
                link_target = (dest.parent / entry.linkname).resolve()
                if not link_target.is_relative_to(target.resolve()):
                    raise ValueError('Dependency symlink leaves its source directory')
                try:
                    dest.symlink_to(entry.linkname)
                except (OSError, NotImplementedError):
                    if os.name != 'nt':
                        raise


                    deferred_windows_links.append((dest, link_target))
    for dest, source in deferred_windows_links:
        if source.is_file():
            shutil.copyfile(source, dest)
        elif source.is_dir():
            shutil.copytree(source, dest)
        else:
            raise ValueError(f'Windows dependency link target missing: {source}')


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, default=root / 'work/runtime-dependency-downloads')
    parser.add_argument('--vulkan', action='store_true', help='Also install pinned native Vulkan UI dependencies')
    args = parser.parse_args()
    args.cache.mkdir(parents=True, exist_ok=True)
    pins = json.loads((root / 'tools/android_runtime_deps.json').read_text())
    if args.vulkan:
        pins['dependencies'] += json.loads((root / 'tools/android_vulkan_deps.json').read_text())['dependencies']
    target = root / 'sdk/thirdparty'
    obtain(pins['sdk_source'], args.cache, target, sdk=True)
    for item in pins['dependencies']:
        source = target / item['name']
        obtain(item, args.cache, source)
        (source / '.git').mkdir(exist_ok=True)
        print(item['name'], item['commit'])


if __name__ == '__main__':
    main()
