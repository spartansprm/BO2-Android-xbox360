#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only


import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk', type=Path, required=True)
    parser.add_argument('--host-sdk-include', type=Path, required=True,
                        help='Target-independent SIMDe/fmt/etc. headers from verified SDK install')
    parser.add_argument('--out', type=Path, default=Path('work/android-generated-probe'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.out.resolve()
    if not output.is_relative_to(root / 'work'):
        parser.error('Keep private objects under the ignored work/ directory')
    properties = (args.ndk / 'source.properties').read_text()
    if not re.search(r'Pkg\.Revision\s*=\s*27\.2\.12479018\b', properties):
        parser.error('This probe is pinned to NDK 27.2.12479018')
    source = root / 'app/generated/default_mp/bo2zm_recomp.0.cpp'
    compiler = args.ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang++'
    output.mkdir(parents=True, exist_ok=True)
    obj = output / 'bo2zm_recomp.0.android.o'
    command = [str(compiler.resolve()), '-std=c++23', '-O1', '-fPIC', '-fno-strict-aliasing',
               '-ffp-model=strict', '-fno-char8_t', '-DSPDLOG_FMT_EXTERNAL',
               '-I', str(root / 'sdk/include'), '-I', str(args.host_sdk_include.resolve()),
               '-I', str(root / 'app/src'), '-include', str(root / 'app/src/bo2zm_guest_memory.h'),
               '-c', str(source), '-o', str(obj)]
    with (output / 'compile.log').open('wb') as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT, timeout=120)
    binary = obj.read_bytes()
    if binary[:6] != b'\x7fELF\x02\x01' or int.from_bytes(binary[18:20], 'little') != 183:
        raise ValueError('Output is not an ELF64 AArch64 object')
    report = {'scope': 'One generated translation unit compiled; no linking or execution',
              'ndk': '27.2.12479018', 'target': 'aarch64-linux-android28',
              'source': source.name, 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
              'generated_functions_in_unit': len(re.findall(r'^DEFINE_REX_FUNC\(', source.read_text(), re.M)),
              'object_bytes': len(binary), 'object_sha256': hashlib.sha256(binary).hexdigest(),
              'elf_machine': 183, 'elf_class': 64, 'optimization': 'O1',
              'linked_runtime': False, 'executed_on_device': False}
    (output / 'compile-result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
