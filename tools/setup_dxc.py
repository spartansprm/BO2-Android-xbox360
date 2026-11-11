#!/usr/bin/env python3


import argparse
import hashlib
from pathlib import Path
import platform
import tarfile
import tempfile
import urllib.request

VERSION = 'v1.8.2505.1'
URL = ('https://github.com/microsoft/DirectXShaderCompiler/releases/download/'
       + VERSION + '/linux_dxc_2025_07_14.x86_64.tar.gz')
SHA256 = 'f2213da1fc99dc8778c8823078e16ba97c7f80f86a1d4520ab1adf4b462bc48c'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'AMD64'):
        parser.error('This pinned package requires a Linux x86-64 cloud host')
    dest = args.out.resolve()
    stamp = dest / '.bo2-dxc-sha256'
    if (dest / 'bin/dxc').is_file() and stamp.is_file() and stamp.read_text().strip() == SHA256:
        print('Pinned DXC already present:', dest / 'bin/dxc')
        return
    if dest.exists() and any(dest.iterdir()):
        parser.error(f'{dest} is not an empty managed installation; move it aside first')
    with tempfile.TemporaryDirectory(prefix='bo2-dxc-') as directory:
        archive = Path(directory) / 'dxc.tar.gz'
        urllib.request.urlretrieve(URL, archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
            raise ValueError('DXC release checksum does not match')
        with tarfile.open(archive) as bundle:
            bundle.extractall(dest, filter='data')
    stamp.write_text(SHA256 + '\n')
    print('DXC installed and SHA-256 verified:', dest / 'bin/dxc')


if __name__ == '__main__':
    main()
