#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only


import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile

EXPECTED_SHA256 = 'ecb2d7ce653be4c4fc8945e0083e222570266b5a770896819427be69d463f72e'
EXPECTED_BYTES = 47054848
MAX_ZIP = 32 * 1024 * 1024
NAMES = {'game/default_mp.xex', 'game/default_mp.xexp', 'manifest.json'}


def image_matches(data):
    return len(data) == EXPECTED_BYTES and hashlib.sha256(data).hexdigest() == EXPECTED_SHA256


def unpack(source, output):
    if source.stat().st_size > MAX_ZIP:
        raise ValueError('Private ZIP exceeds 32 MiB')
    with zipfile.ZipFile(source) as archive:
        entries = archive.infolist()
        if len(entries) != 3 or {entry.filename for entry in entries} != NAMES:
            raise ValueError('ZIP must contain exactly the two fixed game entries and manifest.json')
        if sum(entry.file_size for entry in entries) > MAX_ZIP:
            raise ValueError('Private ZIP expands beyond 32 MiB')
        manifest = json.loads(archive.read('manifest.json'))
        if manifest.get('schema') != 1 or manifest.get('private_game_inputs') is not True:
            raise ValueError('Unexpected private input manifest')
        metadata = {}
        for extension in ('xex', 'xexp'):
            name = f'game/default_mp.{extension}'
            data = archive.read(name)
            digest = hashlib.sha256(data).hexdigest()
            declared = manifest[f'default_mp_{extension}']
            if data[:4] != b'XEX2' or len(data) != declared['bytes'] or digest != declared['sha256']:
                raise ValueError(f'Invalid input or manifest mismatch: {extension}')
            target = output / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            metadata[extension] = {'bytes': len(data), 'sha256': digest}
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('zip', type=Path)
    parser.add_argument('--xexdump', required=True, type=Path)
    parser.add_argument('--out', type=Path, default=Path('work/private-xex-verification'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.out.resolve()
    if not out.is_relative_to(root / 'work'):
        parser.error('Store private inputs and images under the ignored repository work/ directory')
    if out.exists() and any(out.iterdir()):
        parser.error('Use a new output directory to avoid stale images or overwriting prior inputs')
    out.mkdir(parents=True, exist_ok=True)
    inputs = unpack(args.zip, out)
    with (out / 'loader.log').open('wb') as log:
        subprocess.run([str(args.xexdump.resolve()), str(out / 'game'), 'default_mp.xex', str(out / 'dump')],
                       stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60, cwd=out)
    summary = json.loads((out / 'dump/default_mp.summary.json').read_text())
    image = (out / 'dump/default_mp.image.bin').read_bytes()
    matched = image_matches(image)
    metadata_ok = (summary.get('patch_applied') is True and summary.get('title_id') == '415608C3'
                   and summary.get('base_address') == '0x82000000'
                   and summary.get('image_size') == EXPECTED_BYTES
                   and summary.get('version') == '0.0.18.14')
    report = {'schema': 1, 'private_archive_sha256': hashlib.sha256(args.zip.read_bytes()).hexdigest(),
              'inputs': inputs, 'loader': 'ReXGlue official SDK v0.10.0 Linux x86-64, tools/xexdump',
              'sdk_archive_sha256': '0388edb3f8fb1444f1b0ad5d440cb02d9690ac20a76b04fd6b7dcc19f59328a6',
              'image_bytes': len(image), 'image_sha256': hashlib.sha256(image).hexdigest(),
              'reference_image_sha256': EXPECTED_SHA256, 'reference_image_matches': matched,
              'compatible_tu18_image': matched and metadata_ok, 'version': summary.get('version'),
              'image_base': summary.get('base_address'), 'entry_point': summary.get('entry_point'),
              'imports': [{'library': lib['library'], 'count': lib['count']} for lib in summary['imports']],
              'scope': 'Executable image compatibility only; no game execution or Android runtime validation'}
    (out / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    if not report['compatible_tu18_image']:
        raise SystemExit('Image does not match the required reference; codegen remains blocked')


if __name__ == '__main__':
    main()
