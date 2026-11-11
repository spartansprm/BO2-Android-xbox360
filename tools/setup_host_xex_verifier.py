#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only


import argparse
import hashlib
import os
from pathlib import Path
import platform
import shutil
import subprocess
from urllib.request import urlopen
import zipfile

NAME = 'rexglue-sdk-0.10.0-linux-amd64.zip'
SHA256 = '0388edb3f8fb1444f1b0ad5d440cb02d9690ac20a76b04fd6b7dcc19f59328a6'
URL = 'https://github.com/rexglue/rexglue-sdk/releases/download/v0.10.0/' + NAME


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, help='Reuse an already downloaded SDK archive')
    parser.add_argument('--out', type=Path, default=Path('work/host-xex-verifier'))
    args = parser.parse_args()
    if platform.system() != 'Linux' or platform.machine() != 'x86_64':
        parser.error('This pinned host SDK is for Linux x86-64')
    args.out.mkdir(parents=True, exist_ok=True)
    archive = args.archive or args.out / NAME
    if not archive.exists():
        with urlopen(URL, timeout=60) as response, archive.open('wb') as output:
            shutil.copyfileobj(response, output)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise ValueError('SDK archive SHA-256 mismatch')
    with zipfile.ZipFile(archive) as package:
        for entry in package.infolist():
            path = Path(entry.filename)
            if path.is_absolute() or '..' in path.parts or not path.parts or path.parts[0] != 'linux-amd64':
                raise ValueError('Unexpected SDK archive entry')
        libraries = {'librexruntime.so', 'libfmt.a', 'libxxhash.a', 'libdisasm.a'}
        selected = [entry for entry in package.infolist() if
                    entry.filename.startswith('linux-amd64/include/') or
                    (entry.filename.startswith('linux-amd64/lib/') and Path(entry.filename).name in libraries)]
        package.extractall(args.out, members=selected)
        for entry in selected:
            if entry.external_attr >> 16 & 0o111:
                (args.out / entry.filename).chmod(0o755)
    sdk = (args.out / 'linux-amd64').resolve()
    root = Path(__file__).resolve().parents[1]
    executable = (args.out / 'xexdump').resolve()
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++23', '-O2', '-march=x86-64-v2',
                    '-DSPDLOG_FMT_EXTERNAL', '-I', str(sdk / 'include'),
                    str(root / 'tools/xexdump/xexdump.cpp'), '-L', str(sdk / 'lib'),
                    '-Wl,-rpath,' + str(sdk / 'lib'), '-lrexruntime', '-lfmt',
                    '-o', str(executable)], check=True)
    print(executable)


if __name__ == '__main__':
    main()
