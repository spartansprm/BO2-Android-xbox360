#!/usr/bin/env python3


import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
from check_ubo_spirv import inspect_spirv, require_ubo_only


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--dxc', required=True, type=Path)
    parser.add_argument('--spirv-val', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    common = root / 'tools/xenosrecomp/translator/shader_common.h'
    lines = ['// Generated from authored HLSL + BO2 shared header; GPL-3.0-only.',
             '#pragma once', '#include <cstdint>', 'namespace bo2portprobe {',
             'inline constexpr char kCommonSha256[] = "' + hashlib.sha256(common.read_bytes()).hexdigest() + '";']
    cases = [('ubo_vertex', 'vs', 'Vertex'), ('ubo_pixel', 'ps', 'Fragment'),
             ('texture_vertex', 'vs', 'TextureVertex'), ('texture_pixel', 'ps', 'TextureFragment'),
             ('pipeline_vertex', 'vs', 'PipelineVertex'), ('pipeline_pixel', 'ps', 'PipelineFragment')]
    with tempfile.TemporaryDirectory(prefix='bo2-port-shaders-') as directory:
        for name, stage, symbol in cases:
            output = Path(directory) / (name + '.spv')
            subprocess.run([str(args.dxc.resolve()), '-spirv', '-T', stage + '_6_6',
                            '-E', 'main', '-HV', '2021', '-fspv-target-env=vulkan1.2',
                            '-fvk-use-dx-layout', '-DBO2ZM_UBO_ONLY=1', '-I', str(common.parent),
                            '-Fo', str(output), str(root / 'tools/shader_tests' / (name + '.hlsl'))], check=True)
            subprocess.run([str(args.spirv_val.resolve()), '--target-env', 'vulkan1.2', str(output)], check=True)
            data = output.read_bytes()
            require_ubo_only(inspect_spirv(data))
            words = struct.unpack('<' + 'I' * (len(data) // 4), data)
            lines.append(f'inline constexpr char k{symbol}Sha256[] = "{hashlib.sha256(data).hexdigest()}";')
            lines.append(f'inline constexpr uint32_t k{symbol}[] = {{')
            for start in range(0, len(words), 8):
                lines.append('  ' + ', '.join(f'0x{word:08x}' for word in words[start:start + 8]) + ',')
            lines.append('};')
    lines.append('} // namespace bo2portprobe')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text('\n'.join(lines) + '\n')
    print('6 authored probe modules validated: Logical, no Int64, no push constants')


if __name__ == '__main__':
    main()
