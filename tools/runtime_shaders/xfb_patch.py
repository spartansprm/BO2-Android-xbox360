#!/usr/bin/env python3


import re
import subprocess
import sys


def main():
    source, target = sys.argv[1], sys.argv[2]
    text = subprocess.run(['spirv-dis', '--raw-id', source], capture_output=True, text=True, check=True).stdout
    lines = text.splitlines()
    location, position = {}, None
    outputs = set()
    entry = None
    for line in lines:
        m = re.match(r'\s*OpDecorate (%\d+) Location (\d+)', line)
        if m:
            location[m.group(1)] = int(m.group(2))
        m = re.match(r'\s*OpDecorate (%\d+) BuiltIn Position', line)
        if m:
            position = m.group(1)
        m = re.match(r'\s*(%\d+) = OpVariable %\d+ Output', line)
        if m:
            outputs.add(m.group(1))
        m = re.match(r'\s*OpEntryPoint Vertex (%\d+)', line)
        if m:
            entry = m.group(1)
    if not entry or position not in outputs:
        raise SystemExit('not a vertex shader with a plain gl_Position output')
    order = [position] + sorted((v for v in outputs if v in location), key=lambda v: location[v])
    stride = 16 * len(order)
    decorations = []
    for k, var in enumerate(order):
        decorations += [f'OpDecorate {var} XfbBuffer 0', f'OpDecorate {var} XfbStride {stride}',
                        f'OpDecorate {var} Offset {16 * k}']
    out = []
    capability_done = mode_done = decoration_done = False
    for line in lines:
        s = line.strip()
        if s.startswith(';'):
            continue
        if not capability_done and s.startswith('OpCapability'):
            out.append('OpCapability TransformFeedback')
            capability_done = True
        if not decoration_done and s.startswith('OpDecorate'):
            out += decorations
            decoration_done = True
        out.append(line)
        if not mode_done and s.startswith('OpEntryPoint'):
            out.append(f'OpExecutionMode {entry} Xfb')
            mode_done = True
    subprocess.run(['spirv-as', '--preserve-numeric-ids', '--target-env', 'vulkan1.2', '-o', target, '-'],
                   input='\n'.join(out) + '\n', text=True, check=True)
    print(stride)


if __name__ == '__main__':
    main()
