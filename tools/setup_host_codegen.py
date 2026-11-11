#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only


import argparse
import hashlib
import io
from pathlib import Path
import subprocess
import tarfile
from urllib.request import urlopen

SOURCES = (
    ('rexglue/rexglue-sdk', 'c94f5ebdcb3c9d1a460ca48e04f9758448f8d518',
     'f346972da937fbf1561235032cfa06ef16a1f0e994aa3aaf6028fbc51e785a3f'),
    ('pantor/inja', '7d1b4600b68595085a949743331c2e5673f511ea',
     'e86aee0376a9ae726bff9d9309e1319bc6e9ddce3d3204df21953aa6f0d96899'),
)


def git_head(directory):
    result = subprocess.run(['git', '-C', str(directory), 'rev-parse', 'HEAD'], capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else ''


def fetch_with_git(repo, revision, destination):
    destination.mkdir(parents=True, exist_ok=True)
    run = lambda *command: subprocess.run(['git', '-C', str(destination), *command], check=True)
    run('init', '-q')
    run('fetch', '-q', '--depth', '1', f'https://github.com/{repo}.git', revision)
    run('checkout', '-q', 'FETCH_HEAD')
    if git_head(destination) != revision:
        raise ValueError(f'Host dependency commit mismatch: {repo}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host-sdk', type=Path, required=True)
    parser.add_argument('--dependencies', type=Path, default=Path('work/host-codegen-dependencies'))
    parser.add_argument('--out', type=Path, default=Path('work/host-codegen'))
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--ninja', default='ninja')
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    args.dependencies.mkdir(parents=True, exist_ok=True)
    roots = []
    for repo, revision, expected in SOURCES:
        name = repo.split('/')[1] + '-' + revision
        archive = args.dependencies / (name + '.tar.gz')
        destination = args.dependencies / name
        if not archive.exists() and not destination.exists():
            try:
                with urlopen(f'https://codeload.github.com/{repo}/tar.gz/{revision}', timeout=60) as response:
                    archive.write_bytes(response.read())
            except OSError as error:


                print(f'{repo}: archive download failed ({error}); fetching commit {revision} with git')
                fetch_with_git(repo, revision, destination)
        if archive.exists():
            data = archive.read_bytes()
            if hashlib.sha256(data).hexdigest() != expected:
                raise ValueError(f'Host dependency hash mismatch: {repo}')
            if not destination.exists():
                with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as package:
                    package.extractall(args.dependencies, filter='data')
        elif git_head(destination) != revision:
            if (destination / '.git').is_dir():
                fetch_with_git(repo, revision, destination)
            else:
                raise ValueError(f'Host dependency commit mismatch: {repo}')
        roots.append(destination.resolve())
    root = Path(__file__).resolve().parents[1]
    subprocess.run([args.cmake, '-S', str(root / 'tools/host_codegen'), '-B', str(args.out), '-G', 'Ninja',
                    '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_CXX_COMPILER=' + args.cxx,
                    '-DCMAKE_MAKE_PROGRAM=' + args.ninja,
                    '-DBO2_HOST_SDK=' + str(args.host_sdk.resolve()),
                    '-DBO2_HOST_SOURCE=' + str(roots[0]), '-DBO2_HOST_INJA=' + str(roots[1])], check=True)
    subprocess.run([args.cmake, '--build', str(args.out), '-j', str(args.jobs)], check=True)
    print((args.out / 'bo2-codegen').resolve())


if __name__ == '__main__':
    main()
