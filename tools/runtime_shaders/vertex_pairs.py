#!/usr/bin/env python3


import os
import re
import subprocess
import sys


def reflect(path):
    text = subprocess.run(['spirv-dis', '--raw-id', path], capture_output=True, text=True, check=True).stdout
    if 'OpEntryPoint Vertex' not in text:
        return None
    defs, location, builtin = {}, {}, set()
    for line in text.splitlines():
        m = re.match(r'\s*(%\d+) = (Op\w+)(.*)', line)
        if m:
            defs[m.group(1)] = (m.group(2), m.group(3).split())
        m = re.match(r'\s*OpDecorate (%\d+) Location (\d+)', line)
        if m:
            location[m.group(1)] = int(m.group(2))
        m = re.match(r'\s*OpDecorate (%\d+) BuiltIn', line)
        if m:
            builtin.add(m.group(1))

    def scalar_kind(t):
        op, args = defs[t]
        if op == 'OpTypePointer':
            return scalar_kind(args[1])
        if op == 'OpTypeVector':
            return scalar_kind(args[0])
        if op == 'OpTypeInt':
            return 'u' if args[1] == '0' else 'i'
        return 'f'

    inputs, outputs = [], []
    for var, (op, args) in defs.items():
        if op != 'OpVariable' or var not in location or var in builtin:
            continue
        if args[1] == 'Input':
            inputs.append((location[var], scalar_kind(args[0])))
        elif args[1] == 'Output':
            outputs.append(location[var])
    return sorted(inputs), sorted(outputs)


def main():
    pairs, work = sys.argv[1], sys.argv[2]
    os.makedirs(work, exist_ok=True)
    for line in open(pairs):
        parts = line.split()
        if len(parts) != 3:
            continue
        name, a, b = parts
        info = reflect(a)
        if info is None:
            continue
        inputs, outputs = info
        if any(kind == 'i' for _, kind in inputs):
            continue
        body = ''.join(f'layout(location = {loc}) in vec4 v{loc};\n' for loc in outputs)
        mix = ' + '.join(f'v{loc} * {1.0 / (k + 1.37):.6f}' for k, loc in enumerate(outputs)) or 'vec4(0.25)'
        glsl = ('#version 450\n' + body + 'layout(location = 0) out vec4 color;\n'
                'void main() { color = ' + mix + ' + vec4(gl_FragCoord.z); }\n')
        frag = os.path.join(work, name + '.fixed.frag')
        spv = frag + '.spv'
        with open(frag, 'w') as f:
            f.write(glsl)
        subprocess.run(['glslangValidator', '-V', frag, '-o', spv], capture_output=True, check=True)
        listed = ','.join(f'{loc}{"u" if kind == "u" else "f"}' for loc, kind in inputs) or '-'

        here = os.path.dirname(os.path.abspath(__file__))
        strides = []
        for module, tag in ((a, 'a'), (b, 'b')):
            out = os.path.join(work, f'{name}.{tag}.xfb.spv')
            r = subprocess.run([sys.executable, '-I', os.path.join(here, 'xfb_patch.py'), module, out],
                               capture_output=True, text=True)
            strides.append(r.stdout.strip() if r.returncode == 0 else '')
        if strides[0] and strides[0] == strides[1]:
            print(f'{name} {work}/{name}.a.xfb.spv {work}/{name}.b.xfb.spv xfb {spv} {strides[0]} {listed}')
        else:
            print(f'{name} {a} {b} vs {spv} {listed}')


if __name__ == '__main__':
    main()
