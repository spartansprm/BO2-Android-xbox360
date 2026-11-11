#!/usr/bin/env python3


import collections
import os
import re
import subprocess
import sys

SPIRV_DIS = os.environ.get('SPIRV_DIS', 'spirv-dis')
SHADOW_MIN_MARKER = 0x5E3B1A84


def disassemble(path):
    return subprocess.run([SPIRV_DIS, '--raw-id', path], capture_output=True, text=True, check=True).stdout


def interface(text):
    lines = text.splitlines()
    defs, decorations = {}, collections.defaultdict(list)
    for line in lines:
        m = re.match(r'\s*(%\d+) = (Op\w+)(.*)', line)
        if m:
            defs[m.group(1)] = (m.group(2), m.group(3).strip())
        m = re.match(r'\s*OpDecorate (%\d+) (.*)', line)
        if m:
            decorations[m.group(1)].append(m.group(2))

    def type_name(t, depth=0):
        if t not in defs or depth > 8:
            return t
        op, rest = defs[t]
        args = rest.split()
        if op == 'OpTypePointer':
            return 'ptr(%s,%s)' % (args[0], type_name(args[1], depth + 1))
        if op in ('OpTypeVector', 'OpTypeMatrix'):
            return '%s<%s,%s>' % (op[6:], type_name(args[0], depth + 1), args[1])
        if op in ('OpTypeArray', 'OpTypeRuntimeArray'):
            element = type_name(args[0], depth + 1)
            if element.startswith(('img(', 'Sampler')):
                return 'heap<%s>' % element
            return 'arr<%s,%s>' % (element, defs.get(args[1], ('', '?'))[1].split()[-1] if len(args) > 1 else 'rt')
        if op == 'OpTypeImage':
            a = args[1:]
            return 'img(%s)' % ' '.join([a[0], 'd'] + a[2:])
        if op == 'OpTypeStruct':
            return 'struct(%s)' % ','.join(type_name(x, depth + 1) for x in args)
        if op == 'OpTypeInt':
            return 'int%s%s' % (args[0], 's' if args[1] == '1' else 'u')
        if op == 'OpTypeFloat':
            return 'f' + args[0]
        return op[6:]

    out = set()
    for line in lines:
        s = line.strip()
        if s.startswith('OpEntryPoint'):
            out.add('entry ' + s.split()[1])
        elif s.startswith('OpExecutionMode '):
            out.add('mode ' + ' '.join(s.split()[2:]))
        elif s.startswith('OpCapability') and 'RuntimeDescriptorArray' not in s:
            out.add(s)
    for var, (op, rest) in defs.items():
        if op != 'OpVariable':
            continue
        args = rest.split()
        storage = args[1]
        if storage in ('Function', 'Private'):
            continue
        keys = sorted(d for d in decorations.get(var, []) if d.split()[0] in
                      ('Location', 'BuiltIn', 'DescriptorSet', 'Binding', 'Flat', 'NoPerspective', 'Centroid',
                       'Sample', 'Component', 'Index'))
        out.add('var %s [%s] %s' % (storage, ' '.join(keys), type_name(args[0])))
    for target, items in decorations.items():
        for d in items:
            if d.startswith('SpecId'):
                out.add('spec %s %s' % (d, type_name(defs[target][1].split()[0]) if target in defs else ''))
    out.add('kills=%d' % text.count('OpKill'))
    out.add('shadow_min_marker=%s' % (str(SHADOW_MIN_MARKER) in text or hex(SHADOW_MIN_MARKER) in text.lower()))
    return out


def compare(a, b):
    x, y = interface(disassemble(a)), interface(disassemble(b))
    return sorted(x - y), sorted(y - x)


def main():
    if len(sys.argv) == 3 and sys.argv[1] != '--pairs':
        only_a, only_b = compare(sys.argv[1], sys.argv[2])
        for item in only_a:
            print('only in a:', item)
        for item in only_b:
            print('only in b:', item)
        return 1 if only_a or only_b else 0
    if len(sys.argv) == 3:
        same = subset = different = 0
        reasons = collections.Counter()
        examples = []
        for line in open(sys.argv[2]):
            parts = line.split()
            if len(parts) != 3:
                continue
            only_a, only_b = compare(parts[1], parts[2])
            if not only_a and not only_b:
                same += 1
                continue


            if not only_b and all(x.startswith(('var Uniform ', 'var UniformConstant ')) for x in only_a):
                subset += 1
                continue
            different += 1
            if len(examples) < 10:
                examples.append(parts[0])
            for item in only_a:
                reasons['only a: ' + item] += 1
            for item in only_b:
                reasons['only b: ' + item] += 1
        print(f'interfaces identical: {same}; compatible (b uses fewer descriptors): {subset}; different: {different}')
        for reason, count in reasons.most_common(30):
            print(f'  {count} x {reason}')
        if examples:
            print('  examples:', ' '.join(examples))
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main())
