#!/usr/bin/env python3

import argparse
from pathlib import Path
import platform
import struct
import subprocess
import tempfile


def verify(words):
    if len(words) < 5 or words[0] != 0x07230203:
        raise ValueError('Invalid SPIR-V header')
    offset = 5
    while offset < len(words):
        size, opcode = words[offset] >> 16, words[offset] & 0xFFFF
        if not size or offset + size > len(words):
            raise ValueError('Invalid SPIR-V instruction')
        operands = words[offset + 1:offset + size]
        if opcode == 17 and operands[0] in (11, 12, 5347):
            raise ValueError('Probe must not require Int64, Int64Atomics or physical addresses')
        if opcode == 21 and operands[1] == 64:
            raise ValueError('Probe contains a 64-bit integer type')
        if opcode == 14 and operands[0] != 0:
            raise ValueError('Probe must use the Logical addressing model')
        offset += size


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--ndk', required=True, type=Path)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    host = {'Linux': 'linux-x86_64', 'Darwin': 'darwin-x86_64',
            'Windows': 'windows-x86_64'}[platform.system()]
    suffix = '.exe' if platform.system() == 'Windows' else ''
    tools = args.ndk / 'shader-tools' / host
    output = ['// Generated from independent diagnostic GLSL, GPL-3.0-only.',
              '#pragma once', '#include <cstdint>', 'namespace bo2probe {']
    with tempfile.TemporaryDirectory(prefix='bo2-ubo-probe-') as directory:
        for stage, name in [('vert', 'Vertex'), ('frag', 'Fragment')]:
            binary = Path(directory) / f'probe.{stage}.spv'
            subprocess.run([str(tools / ('glslc' + suffix)), '--target-env=vulkan1.1',
                            '-O', str(args.source / f'probe.{stage}'), '-o', str(binary)], check=True)
            subprocess.run([str(tools / ('spirv-val' + suffix)),
                            '--target-env', 'vulkan1.1', str(binary)], check=True)
            data = binary.read_bytes()
            if len(data) % 4:
                raise ValueError('SPIR-V byte count is not word aligned')
            words = struct.unpack('<' + 'I' * (len(data) // 4), data)
            verify(words)
            output.append(f'inline constexpr uint32_t k{name}[] = {{')
            for start in range(0, len(words), 8):
                output.append('  ' + ', '.join(f'0x{w:08x}' for w in words[start:start + 8]) + ',')
            output.append('};')
    output.append('} // namespace bo2probe')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text('\n'.join(output) + '\n')
    print('UBO probe shaders validated: Logical addressing, no Int64')


if __name__ == '__main__':
    main()
