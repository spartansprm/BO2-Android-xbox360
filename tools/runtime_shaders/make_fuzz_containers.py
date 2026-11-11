#!/usr/bin/env python3


import argparse
import random
import struct
from pathlib import Path


def bits(fields):
    pass
    word, at = 0, 0
    for value, width in fields:
        word |= (value & ((1 << width) - 1)) << at
        at += width
    assert at <= 32, at
    return word


def alu(rng, export=None, kill_ok=False, pixel=True, predicated=False, pred_cond=0):
    vector_ops = list(range(0, 30))
    scalar_ops = [o for o in range(0, 51) if o != 41]
    if not kill_ok:
        vector_ops = [o for o in vector_ops if o not in (24, 25, 26, 27)]
        scalar_ops = [o for o in scalar_ops if o not in (35, 36, 37, 38, 39)]
    vop = rng.choice(vector_ops)
    sop = rng.choice(scalar_ops)
    if export is not None:
        vdest, export_bit, vmask = export, 1, 0xF
        smask = 0
    else:
        vdest, export_bit, vmask = rng.randrange(0, 24), 0, rng.randrange(0, 16)
        smask = rng.randrange(0, 16)
    sdest = rng.randrange(0, 24)
    w0 = bits([(vdest, 6), (0, 1), (rng.randrange(2), 1), (sdest, 6), (0, 1), (export_bit, 1),
               (vmask, 4), (smask, 4), (rng.randrange(2), 1), (rng.randrange(2), 1), (sop, 6)])
    w1 = bits([(rng.randrange(256), 8), (rng.randrange(256), 8), (rng.randrange(256), 8),
               (rng.randrange(2), 1), (rng.randrange(2), 1), (rng.randrange(2), 1),
               (pred_cond, 1), (1 if predicated else 0, 1), (0, 1), (0, 1), (0, 1)])

    sel = [rng.randrange(2) for _ in range(3)]
    regs = [rng.randrange(0, 24) if s else rng.randrange(0, 16) for s in sel]
    w2 = bits([(regs[2], 8), (regs[1], 8), (regs[0], 8), (vop, 5), (sel[2], 1), (sel[1], 1), (sel[0], 1)])
    return [w0, w1, w2]


def vertex_fetch(rng, dst, const_index=0, stride=8, offset=0):
    fmt = rng.choice([6, 7, 26, 32, 37, 38, 57, 34, 35, 36])
    w0 = bits([(0, 5), (0, 6), (0, 1), (dst, 6), (0, 1), (1, 1), (const_index, 5), (0, 2), (0, 3), (0, 2)])
    swz = 0
    for c in range(4):
        swz |= rng.choice([0, 1, 2, 3, 4, 5]) << (3 * c)
    w1 = bits([(swz, 12), (rng.randrange(2), 1), (rng.randrange(2), 1), (rng.randrange(2), 1), (0, 1),
               (fmt, 6), (0, 2), (0, 6), (0, 1), (0, 1)])
    w2 = bits([(stride, 8), (offset, 23), (0, 1)])
    return [w0, w1, w2]


def texture_fetch(rng, const_index, dimension, opcode=1, predicated=False, pred_cond=0):
    swz = 0
    for c in range(4):
        swz |= rng.choice([0, 1, 2, 3, 4, 5, 7]) << (3 * c)
    w0 = bits([(opcode, 5), (rng.randrange(0, 24), 6), (0, 1), (rng.randrange(0, 24), 6), (0, 1),
               (0, 1), (const_index, 5), (rng.randrange(2), 1), (rng.randrange(64), 6)])
    w1 = bits([(swz, 12), (rng.randrange(4), 2), (rng.randrange(4), 2), (rng.randrange(4), 2), (0, 3), (0, 3),
               (0, 2), (0, 2), (rng.randrange(2), 1), (rng.randrange(2), 1), (0, 1), (1 if predicated else 0, 1)])
    w2 = bits([(0, 1), (0, 1), (rng.randrange(-8, 8), 7), (0, 5), (dimension, 2),
               (rng.randrange(-4, 4), 5), (rng.randrange(-4, 4), 5), (0, 5), (pred_cond, 1)])
    return [w0, w1, w2]


def cf_exec(opcode, address, count, sequence, bool_address=0, condition=0):
    if opcode in (3, 4):
        a = bits([(address, 12), (count, 3), (0, 1), (sequence, 12), (0, 4)])
        b = bits([(0, 2), (bool_address, 8), (condition, 1), (0, 1), (opcode, 4)])
    else:
        a = bits([(address, 12), (count, 3), (0, 1), (sequence, 12), (0, 4)])
        b = bits([(0, 2), (0, 7), (0, 1), (condition, 1), (0, 1), (opcode, 4)])
    return a, b


def cf_nop():
    return 0, 0


def pack_cf(cfs):
    if len(cfs) % 2:
        cfs = cfs + [cf_nop()]
    words = []
    for (a0, b0), (a1, b1) in zip(cfs[0::2], cfs[1::2]):
        words += [a0, (b0 & 0xFFFF) | ((a1 & 0xFFFF) << 16), (a1 >> 16) | ((b1 & 0xFFFF) << 16)]
    return words


def constant_table(entries):
    pass
    header = 4 + 28
    infos = header
    types = infos + 20 * len(entries)
    names = types + 16 * len(entries)
    blob_names = bytearray()
    name_offsets = []
    for name, *_ in entries:
        name_offsets.append(names - 4 + len(blob_names))
        blob_names += name.encode() + b'\0'
    body = bytearray()
    for i, (name, reg_set, index, count, ptype) in enumerate(entries):
        type_offset = types - 4 + 16 * i
        body += struct.pack('>IHHHHII', name_offsets[i], reg_set, index, count, 0, type_offset, 0)
    type_blob = bytearray()
    for name, reg_set, index, count, ptype in entries:
        pclass = 4 if reg_set == 3 else (0 if reg_set in (0, 1) else 1)
        type_blob += struct.pack('>HHHHHHI', pclass, ptype, 1, 4 if reg_set == 2 else 1, count if reg_set == 2 and count > 1 else 1, 0, 0)
    total = names + len(blob_names)
    table = struct.pack('>IIIIIII', total - 4, 0, 0xFFFE0300, len(entries), infos - 4, 0, 0)
    out = struct.pack('>I', total - 4) + table + body + type_blob + blob_names
    while len(out) % 4:
        out += b'\0'
    return bytes(out)


def make_container(rng, vertex):
    samplers = {}
    instr_words = []
    sequences = []
    pending = []
    vertex_elements = []
    fetch_index = 0
    used_dims = {}

    def add(words, is_fetch):
        pending.append((is_fetch, words))

    regs_written = 0
    body = rng.randrange(3, 24)
    predicated_run = False
    for k in range(body):
        roll = rng.random()
        predicated = rng.random() < 0.15
        cond = rng.randrange(2)
        if vertex and roll < 0.25 and len(vertex_elements) < 6:
            usage = rng.choice([(0, 0), (3, 0), (5, 0), (5, 1), (10, 0), (6, 0), (1, 0)])
            if usage in [(u, i) for _, u, i in vertex_elements]:
                add(alu(rng, pixel=False, predicated=predicated, pred_cond=cond), False)
                continue
            vertex_elements.append((None, usage[0], usage[1]))
            add(vertex_fetch(rng, rng.randrange(0, 24), offset=0), True)
            vertex_elements[-1] = (len(pending) - 1, usage[0], usage[1])
        elif roll < 0.35:
            slot = rng.randrange(0, 8)
            dim = used_dims.setdefault(slot, rng.choice([1, 1, 1, 2, 3]))
            samplers[slot] = dim
            opcode = rng.choice([1, 1, 1, 1, 24, 19, 17])
            add(texture_fetch(rng, slot, dim, opcode, predicated, cond), True)
        else:
            add(alu(rng, kill_ok=not vertex and rng.random() < 0.1, pixel=not vertex,
                    predicated=predicated, pred_cond=cond), False)

    exports = 0
    if vertex:
        add(alu(rng, export=62, pixel=False), False)
        exports = rng.randrange(0, 4)
        for i in range(exports):
            add(alu(rng, export=i, pixel=False), False)
    else:
        add(alu(rng, export=0), False)


    blocks = []
    i = 0
    while i < len(pending):
        n = min(rng.randrange(1, 7), len(pending) - i)
        blocks.append(pending[i:i + n])
        i += n
    cf_count = len(blocks)
    cf_words = (cf_count + 1) // 2 * 3
    base = cf_words // 3
    cfs = []
    address = base
    for b, block in enumerate(blocks):
        last = b == len(blocks) - 1
        seq = 0
        for j, (is_fetch, _) in enumerate(block):
            seq |= (1 if is_fetch else 0) << (2 * j)
        kind = rng.random()
        if last:
            cfs.append(cf_exec(2, address, len(block), seq))
        elif kind < 0.15:
            cfs.append(cf_exec(3, address, len(block), seq, bool_address=rng.randrange(0, 4) + (0 if vertex else 128), condition=rng.randrange(2)))
        elif kind < 0.25:
            cfs.append(cf_exec(5, address, len(block), seq, condition=rng.randrange(2)))
        else:
            cfs.append(cf_exec(1, address, len(block), seq))
        address += len(block)
    microcode = pack_cf(cfs)
    instruction_number = {}
    for block in blocks:
        for is_fetch, words in block:
            instruction_number[id(words)] = len(microcode) // 3
            microcode += words


    elements = []
    for pending_index, usage, usage_index in vertex_elements:
        words = pending[pending_index][1]
        elements.append(bits([(instruction_number[id(words)], 12), (usage, 4), (usage_index, 4)]))

    entries = [('g_C', 2, 0, 16, 3)]
    for b in range(4):
        entries.append((f'g_B{b}', 0, b, 1, 1))
    for slot, dim in sorted(samplers.items()):
        ptype = {1: 12, 2: 13, 3: 14}[dim]
        name = 'SHADOWMAP_SAMPLER' if (not vertex and slot == 7 and dim == 1) else f's{slot}'
        entries.append((name, 3, slot, 1, ptype))
    table = constant_table(entries)

    interpolators = []
    n_interp = max(exports if vertex else 0, rng.randrange(1, 5))
    for i in range(n_interp):
        interpolators.append(bits([(i, 4), (5, 4), (i, 4), (0, 20)]))

    if vertex:
        shader = struct.pack('>IIIIII', 0, len(microcode) * 4, 0, 0, 0, n_interp << 5)
        shader += struct.pack('>III', 0, len(elements), 0)
        shader += b''.join(struct.pack('>I', e) for e in elements)
        shader += b''.join(struct.pack('>I', v) for v in interpolators)
    else:
        sv_pos = rng.choice([0xFF, 1])
        shader = struct.pack('>IIIIII', 0, len(microcode) * 4, 0, sv_pos << 8, 0, n_interp << 5)
        shader += struct.pack('>II', 0, 1)
        shader += b''.join(struct.pack('>I', v) for v in interpolators)

    header_size = 36
    ct_offset = header_size
    shader_offset = ct_offset + len(table)
    virtual_size = shader_offset + len(shader)
    physical = b''.join(struct.pack('>I', w) for w in microcode)
    header = struct.pack('>IIIIIIIII', 0x102A1100 | (1 if vertex else 0), virtual_size, len(physical), 0,
                         ct_offset, 0, shader_offset, 0, 0)
    return header + table + shader + physical


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out', type=Path)
    ap.add_argument('--count', type=int, default=400)
    ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(a.seed)
    for i in range(a.count):
        vertex = i % 2 == 1
        data = make_container(rng, vertex)
        (a.out / f"{'v' if vertex else 'p'}{i:05d}.bin").write_bytes(data)
    print(f'{a.count} containers in {a.out}')


if __name__ == '__main__':
    main()
