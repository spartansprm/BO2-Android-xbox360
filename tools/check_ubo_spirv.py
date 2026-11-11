#!/usr/bin/env python3


import argparse
import json
from pathlib import Path
import struct


def inspect_spirv(data):
    if len(data) % 4 or len(data) < 20:
        raise ValueError('SPIR-V size is invalid')
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    if words[0] != 0x07230203:
        raise ValueError('SPIR-V magic is invalid')
    result = {'capabilities': [], 'integer_widths': [], 'addressing_model': None,
              'push_constants': False}
    offset = 5
    while offset < len(words):
        size, opcode = words[offset] >> 16, words[offset] & 0xFFFF
        if not size or offset + size > len(words):
            raise ValueError('SPIR-V instruction is invalid')
        args = words[offset + 1:offset + size]
        if opcode == 17:
            if len(args) != 1:
                raise ValueError('Malformed OpCapability')
            result['capabilities'].append(args[0])
        elif opcode == 21:
            if len(args) != 3:
                raise ValueError('Malformed OpTypeInt')
            result['integer_widths'].append(args[1])
        elif opcode == 14:
            if len(args) != 2:
                raise ValueError('Malformed OpMemoryModel')
            result['addressing_model'] = args[0]
        elif opcode == 59:
            if len(args) not in (3, 4):
                raise ValueError('Malformed OpVariable')
            result['push_constants'] |= args[2] == 9
        offset += size
    return result


def require_ubo_only(info):
    if {11, 12, 5347} & set(info['capabilities']):
        raise ValueError('Int64, Int64Atomics or physical-storage addressing capability remains')
    if 64 in info['integer_widths']:
        raise ValueError('A 64-bit integer type remains')
    if info['addressing_model'] != 0:
        raise ValueError('The addressing model must be Logical')
    if info['push_constants']:
        raise ValueError('Push constants remain in the UBO-only module')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('modules', nargs='+', type=Path)
    args = parser.parse_args()
    for path in args.modules:
        info = inspect_spirv(path.read_bytes())
        require_ubo_only(info)
        print(path.name, json.dumps(info, sort_keys=True))


if __name__ == '__main__':
    main()
