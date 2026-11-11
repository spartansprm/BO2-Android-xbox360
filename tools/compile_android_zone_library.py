#!/usr/bin/env python3


import argparse
import concurrent.futures
import csv
import ctypes
import ctypes.util
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import time

from check_ubo_spirv import inspect_spirv, require_ubo_only

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def atomic_json(path, value):
    part = path.with_suffix(path.suffix + '.part')
    part.write_text(json.dumps(value, separators=(',', ':')) + '\n')
    os.replace(part, path)


class XXH3:
    def __init__(self):
        self.lib = ctypes.CDLL(ctypes.util.find_library('xxhash'))
        self.lib.XXH3_64bits.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        self.lib.XXH3_64bits.restype = ctypes.c_uint64
        self.lib.XXH3_createState.restype = ctypes.c_void_p
        self.lib.XXH3_freeState.argtypes = [ctypes.c_void_p]
        self.lib.XXH3_64bits_reset.argtypes = [ctypes.c_void_p]
        self.lib.XXH3_64bits_update.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
        self.lib.XXH3_64bits_digest.argtypes = [ctypes.c_void_p]
        self.lib.XXH3_64bits_digest.restype = ctypes.c_uint64
        self.state = self.lib.XXH3_createState()
        if not self.state or self.lib.XXH3_64bits_reset(self.state):
            raise RuntimeError('XXH3 initialization failed')

    def one(self, data):
        return self.lib.XXH3_64bits(data, len(data))

    def update(self, data):
        if self.lib.XXH3_64bits_update(self.state, data, len(data)):
            raise RuntimeError('XXH3 update failed')

    def value(self):
        return self.lib.XXH3_64bits_digest(self.state)

    def __del__(self):
        if getattr(self, 'state', None):
            self.lib.XXH3_freeState(self.state)


def run(command, log):
    with log.open('w') as stream:
        completed = subprocess.run([str(v) for v in command], stdout=stream, stderr=subprocess.STDOUT)
    if completed.returncode:
        raise RuntimeError(f'{Path(str(command[0])).name} failed ({completed.returncode}): {log.read_text()[-5000:]}')


def validate(module, args):
    run([args.spirv_val, '--target-env', 'vulkan1.2', '--scalar-block-layout', module],
        module.with_suffix('.validation.log'))
    data = module.read_bytes()
    if len(data) > 4 * 1024 * 1024:
        raise ValueError('SPIR-V exceeds renderer entry limit')
    require_ubo_only(inspect_spirv(data))
    return data


def build(args):
    out = args.out.resolve()
    if out.is_relative_to(ROOT):
        raise ValueError('Private output must be outside the repository')
    out.mkdir(parents=True, exist_ok=True)
    classification = json.loads(args.inventory.read_text())
    if not classification['transport_complete'] or classification['status'] != 'extracted_and_classified':
        raise ValueError('Incomplete extraction inventory')
    selected = [r for r in classification['containers']
                if not r['cpu_memory_export'] and (r['in_previous_library'] or {'zm', 'mp'} & set(r['modes']))]
    xx = XXH3()
    for row in selected:
        data = (args.containers / row['file']).read_bytes()
        if len(data) != row['bytes'] or hashlib.sha256(data).hexdigest() != row['sha256']:
            raise ValueError('Container content mismatch: ' + row['file'])
        row['container_hash'] = xx.one(data)
    selected.sort(key=lambda r: (r['container_hash'], (args.containers / r['file']).read_bytes()))
    profile = {'inventory_sha256': digest(args.inventory), 'previous_pack_sha256': digest(args.previous_pack),
               'common_header_sha256': digest(args.common), 'translator_sha256': digest(args.translator),
               'dxc_sha256': digest(args.dxc), 'spirv_val_sha256': digest(args.spirv_val),
               'shader_count': len(selected), 'profile': 'ubo-only', 'batch_size': args.batch_size}
    expected = struct.unpack('<8sIIQ', args.previous_pack.open('rb').read(24))
    old_hash = XXH3()
    with args.previous_pack.open('rb') as stream:
        stream.seek(24)
        for block in iter(lambda: stream.read(1024*1024), b''):
            old_hash.update(block)
    if expected[:2] != (b'BZMSPV\0\0', 1) or expected[3] != old_hash.value():
        raise ValueError('Previous pack integrity failure')
    checkpoint = out / 'compile-checkpoint.json'
    part = out / 'bo2zm_shaders.spvpack.part'
    final = out / 'bo2zm_shaders.spvpack'
    if final.exists():
        raise ValueError('Final pack already exists; inspect its build report')
    state = {'profile': profile, 'done': [], 'end': 24, 'hlsl_cache': {}, 'compiled': 0,
             'reused': 0, 'deduplicated': 0, 'status': 'compiling'}
    if checkpoint.exists():
        state = json.loads(checkpoint.read_text())
        if state['profile'] != profile:
            raise ValueError('Checkpoint input/toolchain/profile mismatch')
        if [r['file'] for r in state['done']] != [r['file'] for r in selected[:len(state['done'])]]:
            raise ValueError('Checkpoint ordering mismatch')
    if part.exists():
        with part.open('r+b') as pack:
            if pack.seek(0, 2) < state['end']:
                raise ValueError('Checkpoint pack is truncated')
            pack.truncate(state['end']); pack.seek(24)
            for block in iter(lambda: pack.read(1024*1024), b''):
                xx.update(block)
        if state.get('body_hash') is not None and xx.value() != state['body_hash']:
            raise ValueError('Checkpoint body integrity failure')
    else:
        if state['done']:
            raise ValueError('Checkpoint pack is missing')
        part.write_bytes(struct.pack('<8sIIQ', b'BZMSPV\0\0', 1, len(selected), 0))
    started = time.monotonic()
    initial = len(state['done'])
    with part.open('r+b') as pack, args.previous_pack.open('rb') as previous:
        for start in range(initial, len(selected), args.batch_size):
            batch = selected[start:start+args.batch_size]
            scratch = out / 'batch'
            if scratch.exists():
                shutil.rmtree(scratch)
            inputs = scratch / 'inputs'; inputs.mkdir(parents=True)
            modules = scratch / 'spirv'; modules.mkdir()
            new = [r for r in batch if not r['in_previous_library']]
            for row in new:
                os.link(args.containers / row['file'], inputs / row['file'])
            hlsl = scratch / 'hlsl'
            if new:
                run([args.translator, inputs, hlsl, args.common], scratch / 'translation.log')
                with (hlsl / 'translate.tsv').open() as stream:
                    translations = list(csv.DictReader(stream, delimiter='\t'))
                if (len(translations) != len(new) or any(r['status'] != 'ok' for r in translations)
                        or {r['name'] for r in translations} != {Path(r['file']).stem for r in new}):
                    raise ValueError('Translation incomplete: ' + str([r for r in translations if r['status'] != 'ok']))
            source_for, groups = {}, {}
            for row in new:
                name = Path(row['file']).stem
                source = hlsl / (('v_' if name.startswith('vs_') else 'p_') + name + '.hlsl')
                key = digest(source)
                row['hlsl_sha256'] = key
                source_for[row['file']] = source
                if key not in state['hlsl_cache']:
                    groups.setdefault(key, row)

            def compile_one(row):
                source = source_for[row['file']]
                target = modules / (source.stem + '.spv')
                vertex = row['file'].startswith('vs_')
                run([args.dxc, '-spirv', '-T', 'vs_6_6' if vertex else 'ps_6_6', '-E', 'main',
                     '-HV', '2021', '-fspv-target-env=vulkan1.2', '-fvk-use-dx-layout',
                     '-DBO2ZM_UBO_ONLY=1', *(['-fvk-invert-y'] if vertex else []),
                     '-Fo', target, source], target.with_suffix('.dxc.log'))
                validate(target, args)
                return row['hlsl_sha256'], target

            with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
                compiled = dict(pool.map(compile_one, groups.values()))
            pack.seek(state['end'])
            for row in batch:
                if row['in_previous_library']:
                    old = row['previous_spirv']
                    previous.seek(old['spirv_offset']); code = previous.read(old['spirv_bytes'])
                    if hashlib.sha256(code).hexdigest() != old['spirv_sha256']:
                        raise ValueError('Previous SPIR-V mismatch')
                    target = modules / (row['file'] + '.spv'); target.write_bytes(code)
                    validate(target, args)
                    state['reused'] += 1
                else:
                    key = row['hlsl_sha256']
                    if key in state['hlsl_cache']:
                        cached = state['hlsl_cache'][key]
                        end = pack.tell(); pack.seek(cached['offset']); code = pack.read(cached['bytes']); pack.seek(end)
                        if hashlib.sha256(code).hexdigest() != cached['sha256']:
                            raise ValueError('Deduplicated SPIR-V integrity failure')
                        state['deduplicated'] += 1
                    else:
                        code = compiled[key].read_bytes()
                        state['compiled'] += 1
                original = (args.containers / row['file']).read_bytes()
                offset = pack.tell() + 16 + len(original)
                entry = struct.pack('<IIQ', len(original), len(code)//4, row['container_hash']) + original + code
                if pack.tell() + len(entry) > 2*1024*1024*1024:
                    raise ValueError('Pack exceeds renderer file limit')
                pack.write(entry); xx.update(entry)
                record = {'file': row['file'], 'spirv_offset': offset, 'spirv_bytes': len(code),
                          'spirv_sha256': hashlib.sha256(code).hexdigest()}
                state['done'].append(record)
                if not row['in_previous_library']:
                    state['hlsl_cache'][row['hlsl_sha256']] = {
                        'offset': offset, 'bytes': len(code), 'sha256': record['spirv_sha256']}
            pack.flush(); os.fsync(pack.fileno())
            state['end'] = pack.tell(); state['body_hash'] = xx.value()
            atomic_json(checkpoint, state)
            shutil.rmtree(scratch)
            elapsed = time.monotonic() - started
            print(json.dumps({'done': len(state['done']), 'total': len(selected), 'pack_bytes': state['end'],
                              'compiled': state['compiled'], 'reused': state['reused'],
                              'deduplicated': state['deduplicated'], 'elapsed_s': round(elapsed, 1)}), flush=True)
        pack.seek(16); pack.write(struct.pack('<Q', xx.value())); pack.flush(); os.fsync(pack.fileno())
    os.replace(part, final)
    report = {**profile, 'status': 'ready', 'unique_containers': len(selected), 'validated_modules': len(selected),
              'pack_sha256': digest(final), 'pack_bytes': final.stat().st_size,
              'compiled_new_ir': state['compiled'], 'deduplicated_ir': state['deduplicated'],
              'previous_ir_revalidated': state['reused'], 'all_selected_containers_present': True,
              'scope': 'All ZM/MP .ff shaders in the complete private 645-file base/TU18/DLC export; '
                       'previous 6237 retained; campaign-only excluded; physical rendering unverified',
              'source_run_id': classification['source_run_id']}
    atomic_json(out / 'shader-build-results.json', report)
    print(json.dumps(report), flush=True)


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--inventory', type=Path, required=True)
    ap.add_argument('--containers', type=Path, required=True)
    ap.add_argument('--previous-pack', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--translator', type=Path, required=True)
    ap.add_argument('--dxc', type=Path, required=True)
    ap.add_argument('--spirv-val', type=Path, required=True)
    ap.add_argument('--common', type=Path, default=ROOT/'tools/xenosrecomp/translator/shader_common.h')
    ap.add_argument('--batch-size', type=int, default=128)
    ap.add_argument('--jobs', type=int, default=2)
    build(ap.parse_args())
