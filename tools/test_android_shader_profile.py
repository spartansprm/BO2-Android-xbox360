#!/usr/bin/env python3


import argparse
import hashlib
import json
from pathlib import Path
from check_ubo_spirv import inspect_spirv, require_ubo_only
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--dxc', type=Path, required=True)
    parser.add_argument('--spirv-val', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--common', type=Path,
                        default=Path(__file__).resolve().parents[0] / 'xenosrecomp/translator/shader_common.h')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    args.out.mkdir(parents=True, exist_ok=True)
    cases = [('ubo_vertex', 'vs'), ('ubo_pixel', 'ps'), ('texture_vertex', 'vs'),
             ('shared_pixel', 'ps'), ('texture_pixel', 'ps'), ('texcoord_vertex', 'vs')]
    summary = []
    for name, stage in cases:
        for profile in ['default', 'ubo-only']:
            output = args.out / f'{name}.{profile}.spv'
            command = [str(args.dxc.resolve()), '-spirv', '-T', stage + '_6_6',
                       '-E', 'main', '-HV', '2021', '-fspv-target-env=vulkan1.2',
                       '-fvk-use-dx-layout', '-I', str(args.common.resolve().parent),
                       '-Fo', str(output), str(root / 'tools/shader_tests' / f'{name}.hlsl')]
            if profile == 'ubo-only':
                command.append('-DBO2ZM_UBO_ONLY=1')
            subprocess.run(command, check=True)
            subprocess.run([str(args.spirv_val.resolve()), '--target-env', 'vulkan1.2',
                            '--scalar-block-layout', str(output)], check=True)
            data = output.read_bytes()
            info = inspect_spirv(data)
            if profile == 'ubo-only':
                require_ubo_only(info)
            else:
                assert 11 in info['capabilities'], info
                assert info['addressing_model'] == 5348, info
                assert info['push_constants'], info
            summary.append({'case': name, 'profile': profile, 'bytes': len(data),
                            'sha256': hashlib.sha256(data).hexdigest(), **info})
            print(name, profile, 'PASS', info)
    (args.out / 'shader-profile-results.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(f'{len(summary)} modules compiled and validated; {len(cases)} UBO-only modules without Int64')


if __name__ == '__main__':
    main()
