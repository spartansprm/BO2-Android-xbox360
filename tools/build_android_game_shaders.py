#!/usr/bin/env python3


import argparse
import csv
import hashlib
import json
import multiprocessing
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zipfile

from check_ubo_spirv import inspect_spirv, require_ubo_only
import importlib.util

ROOT = Path(__file__).resolve().parents[1]
MAX_BYTES = 28 * 1024 * 1024
IMAGE_SHA = 'ecb2d7ce653be4c4fc8945e0083e222570266b5a770896819427be69d463f72e'
NAMES = {'code_post_gfx_zm', 'patch_ui_zm', 'patch_zm', 'frontend_patch',
         'patch', 'common_zm', 'weapon_attach_shared', 'code_pre_gfx_zm'}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def validate_batch(path):
    pass
    if path.stat().st_size > 30 * 1024 * 1024:
        raise ValueError('Resource ZIP exceeds 30 MiB')
    with zipfile.ZipFile(path) as archive:
        entries = archive.infolist()
        names = [item.filename for item in entries]
        if len(names) != len(set(names)) or not 2 <= len(names) <= 17:
            raise ValueError('Duplicate names or invalid resource count')
        if any(item.flag_bits & 1 or item.is_dir() for item in entries):
            raise ValueError('Encrypted ZIP entries or directories are unsupported')
        info = archive.getinfo('resource_manifest.json')
        if info.file_size > 256 * 1024:
            raise ValueError('Resource manifest exceeds limit')
        manifest = json.loads(archive.read(info))
        if manifest.get('schema') != 1 or manifest.get('private_game_resources') is not True:
            raise ValueError('Expected a private 0.9 resource export')
        vfs = manifest['native_vfs']
        if not vfs.get('passed') or not vfs.get('native_inputs_match_previous_preparation'):
            raise ValueError('Phone VFS/input verification did not pass')
        payloads, total = {}, 0
        for row in vfs['exports']:
            name = row['file']
            if (not re.fullmatch(r'zones/(game|update)/[a-z0-9_]+\.ff', name)
                    or Path(name).stem not in NAMES or name in payloads):
                raise ValueError('Unexpected resource path or duplicate export')
            item = archive.getinfo(name)
            if item.file_size != row['bytes'] or not 316 <= item.file_size <= MAX_BYTES - total:
                raise ValueError('Resource size/budget mismatch')
            data = archive.read(item)
            if sha(data) != row['sha256']:
                raise ValueError('Resource SHA-256 mismatch')
            if (data[:8] not in (b'TAffx100', b'TAff0100') or int.from_bytes(data[8:12], 'big') != 146
                    or data[12:20] != b'PHEEBs71'):
                raise ValueError('Unsupported resource container header')
            payloads[name] = data
            total += len(data)
        if not payloads or set(names) != {'resource_manifest.json', *payloads}:
            raise ValueError('Empty batch or unlisted ZIP entries')
        return manifest, payloads


def run(command, log):
    with log.open('w') as output:
        subprocess.run([str(arg) for arg in command], stdout=output,
                       stderr=subprocess.STDOUT, check=True)


def build_tools(out, cxx):
    translator = ROOT / 'tools/xenosrecomp/translator'
    thirdparty = ROOT / 'sdk/thirdparty'
    hlsl, packer = out / 'bo2zm_hlsl', out / 'bo2zm_pack'
    run([cxx, '-std=c++23', '-O1', '-w', '-I'+str(translator),
         '-I'+str(thirdparty/'fmt/include'), '-I'+str(thirdparty/'xxHash'),
         '-include', translator/'pch_min.h', '-include', ROOT/'tools/xenosrecomp/assert_throw.h',
         '-DFMT_HEADER_ONLY', '-DXXH_INLINE_ALL', '-DBO2ZM_RECOMP',
         ROOT/'tools/xenosrecomp/bo2zm_hlsl.cpp', translator/'shader_recompiler.cpp',
         '-o', hlsl], out/'build-hlsl.log')
    render = ROOT / 'app/src/native/render'
    run([cxx, '-std=c++23', '-O1', '-I'+str(render), '-I'+str(thirdparty/'xxHash'),
         ROOT/'tools/xenosrecomp/bo2zm_pack.cpp', render/'bo2zm_shader_library.cpp',
         render/'bo2zm_shader_index.cpp', '-o', packer], out/'build-packer.log')
    return hlsl, packer


def process(args):
    manifest, payloads = validate_batch(args.private_zip)
    out = args.out.resolve()
    if out.is_relative_to(ROOT) or out.exists():
        raise ValueError('Output must be a NEW directory outside the source tree')
    if sha(args.xex_image.read_bytes()) != IMAGE_SHA:
        raise ValueError('Expected the previously verified patched TU18 image')
    spec = importlib.util.spec_from_file_location('bo2_ff_shaders', ROOT/'tools/inspect/ff_shaders.py')
    ff = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = ff
    spec.loader.exec_module(ff)
    key, _ = ff.key_from_xex_image(str(args.xex_image))
    out.mkdir(parents=True)
    report = {'status': 'working', 'scope': 'partial boot-zone library; no gameplay coverage claim',
              'profile': 'ubo-only', 'private_game_resources': True,
              'source_zip_sha256': sha(args.private_zip.read_bytes()), 'zones': []}
    try:
        blobs = out/'containers'; blobs.mkdir()
        unique = set()
        with multiprocessing.get_context('fork').Pool(4) as pool:
            for name, data in payloads.items():
                path = out/name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)
                info, zone = ff.decode_zone(str(path), key, pool)
                if len(zone) < 40 or len(zone) > 256*1024*1024 or ff.parse_xfile(zone)['size'] != len(zone)-40:
                    raise ValueError('Decoded XFile size mismatch: '+name)
                records = ff.find_shaders(zone)
                valid = [row for row in records if row['ok']]
                report['zones'].append({'file': name, 'decoded_bytes': len(zone),
                                        'matched_shaders': len(valid), 'unmatched_candidates': len(records)-len(valid)})
                print(f"Decoded {name}: {len(zone)} bytes, {len(valid)} shader records", flush=True)
                for row in valid:
                    blob = row['blob']
                    if len(blob) != row['virtual']+row['physical']:
                        raise ValueError('Truncated shader container')
                    filename = row['kind']+'_'+hashlib.sha1(blob).hexdigest()[:16]+'.bin'
                    target = blobs/filename
                    if filename in unique and target.read_bytes() != blob:
                        raise ValueError('Shader filename hash collision')
                    target.write_bytes(blob); unique.add(filename)
        report['unique_containers'] = len(unique)
        print(f"{len(unique)} unique containers; building translator and packer", flush=True)
        if not unique:
            raise ValueError('No shaders in this batch; request the next boot zone from the catalogue')
        hlsl, packer = build_tools(out, args.cxx)
        common = ROOT/'tools/xenosrecomp/translator/shader_common.h'
        run([hlsl, blobs, out/'hlsl', common], out/'translate.log')
        with (out/'hlsl/translate.tsv').open() as stream:
            rows = list(csv.DictReader(stream, delimiter='\t'))
        if len(rows) != len(unique) or any(row['status'] != 'ok' for row in rows):
            raise ValueError('Shader translation incomplete; inspect hlsl/translate.tsv')
        spirv = out/'spirv'; spirv.mkdir()
        modules = sorted((out/'hlsl').glob('*.hlsl'))
        if len(modules) != len(unique):
            raise ValueError('Translation output count mismatch')
        capabilities = set()
        for number, path in enumerate(modules, 1):
            vertex = path.name.startswith('v_')
            module = spirv/(path.stem+'.spv')
            run([args.dxc, '-spirv', '-T', 'vs_6_6' if vertex else 'ps_6_6', '-E', 'main',
                 '-HV', '2021', '-fspv-target-env=vulkan1.2', '-fvk-use-dx-layout',
                 '-DBO2ZM_UBO_ONLY=1', *(['-fvk-invert-y'] if vertex else []),
                 '-Fo', module, path], spirv/(path.stem+'.dxc.log'))
            run([args.spirv_val, '--target-env', 'vulkan1.2', '--scalar-block-layout', module],
                spirv/(path.stem+'.validation.log'))
            info = inspect_spirv(module.read_bytes())
            try:
                require_ubo_only(info)
            except ValueError as error:
                raise ValueError(f'{path.stem}: {error}') from error
            capabilities.update(info['capabilities'])
            if number % 100 == 0 or number == len(modules):
                print(f"Compiled and validated {number}/{len(modules)} modules", flush=True)
        pack = out/'bo2zm_shaders.spvpack'
        run([packer, blobs, spirv, pack], out/'pack.log')
        run([packer, '--index', pack], out/'index.log')
        report.update(status='ready', validated_modules=len(modules),
                      stages={'vertex': sum(p.name.startswith('v_') for p in modules),
                              'pixel': sum(p.name.startswith('p_') for p in modules)},
                      capabilities=sorted(capabilities), pack_bytes=pack.stat().st_size,
                      pack_sha256=sha(pack.read_bytes()),
                      index_sha256=sha(pack.with_suffix('.spvidx').read_bytes()),
                      common_header_sha256=sha(common.read_bytes()),
                      dxc_sha256=sha(args.dxc.read_bytes()),
                      spirv_val_sha256=sha(args.spirv_val.read_bytes()))
    except Exception as error:
        report.update(status='error', error=str(error))
        raise
    finally:
        (out/'shader-build-results.json').write_text(json.dumps(report, indent=2)+'\n')
    print(f"{len(modules)} UBO-only modules packed and indexed; partial boot coverage: {out}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--private-zip', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--xex-image', type=Path, default=ROOT/'work/dump_tu18/default_mp.image.bin')
    ap.add_argument('--dxc', required=True, type=Path)
    ap.add_argument('--spirv-val', required=True, type=Path)
    ap.add_argument('--cxx', default=shutil.which('g++-14') or 'g++')
    process(ap.parse_args())


if __name__ == '__main__':
    main()
