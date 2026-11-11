#!/usr/bin/env python3


import argparse
import hashlib
import json
import os
from pathlib import Path
import zipfile


def package_library(source, output, chunk_bytes=96*1024*1024):
    report = json.loads((source/'shader-build-results.json').read_text())
    if (report.get('status') != 'ready' or report.get('profile') != 'ubo-only'
            or not 1 <= report['validated_modules'] <= 65536
            or report['validated_modules'] != report['unique_containers']):
        raise ValueError('Expected a complete validated Android shader build')
    if not 1 <= chunk_bytes <= 96*1024*1024 or output.exists():
        raise ValueError('Expected a new output folder and bounded chunk size')
    output.mkdir(parents=True)
    manifest = {'schema': 1, 'type': 'bo2_android_compiled_shaders', 'status': 'ready',
                'profile': 'ubo-only', 'private_game_resources': True, 'shaders': report['validated_modules'],
                'source_run_id': report.get('source_run_id', ''), 'scope': report.get('scope', ''), 'files': []}
    sequence = 0
    try:
        for filename, hash_key, maximum in [('bo2zm_shaders.spvpack', 'pack_sha256', 2*1024**3),
                                            ('bo2zm_shaders.spvidx', 'index_sha256', 8*1024**2)]:
            path = source/filename
            if not 24 <= path.stat().st_size <= maximum:
                raise ValueError('File length outside the Android importer bounds')
            row = {'file': filename, 'bytes': path.stat().st_size, 'sha256': report[hash_key], 'chunks': []}
            offset, whole = 0, hashlib.sha256()
            with path.open('rb') as stream:
                while payload := stream.read(chunk_bytes):
                    sequence += 1
                    name = f'bo2-biblioteca-{sequence:04d}.zip'
                    target = output/name; part = output/(name+'.part')
                    with zipfile.ZipFile(part, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=1) as archive:
                        archive.writestr('payload.bin', payload)
                    if part.stat().st_size > 100*1024*1024:
                        raise ValueError('Transport ZIP exceeds importer limit')
                    with part.open('rb') as zipped:
                        zip_hash = hashlib.file_digest(zipped, 'sha256').hexdigest()
                    os.replace(part, target)
                    row['chunks'].append({'zip': name, 'zip_bytes': target.stat().st_size, 'zip_sha256': zip_hash,
                                          'offset': offset, 'bytes': len(payload), 'sha256': hashlib.sha256(payload).hexdigest()})
                    offset += len(payload); whole.update(payload)
            if offset != row['bytes'] or whole.hexdigest() != row['sha256']:
                raise ValueError('Validated shader file SHA-256 mismatch')
            manifest['files'].append(row)
        (output/'biblioteca-shaders.json').write_text(json.dumps(manifest, indent=2)+'\n')
    except Exception:

        (output/'biblioteca-shaders.json').unlink(missing_ok=True)
        raise
    return manifest


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--input', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    result = package_library(args.input, args.out)
    print(json.dumps({'shaders': result['shaders'], 'zips': sum(len(f['chunks']) for f in result['files']),
                      'raw_bytes': sum(f['bytes'] for f in result['files'])}))
