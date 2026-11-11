#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only


import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import re
import struct
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

EXPECTED = 'ecb2d7ce653be4c4fc8945e0083e222570266b5a770896819427be69d463f72e'
MAX_IMAGE = 128 * 1024 * 1024


def take(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError('Out-of-range input or image reference')
    return bytes(data[offset:offset + size])


def number(data, offset, fmt='>I'):
    return struct.unpack(fmt, take(data, offset, struct.calcsize(fmt)))[0]


def opt(data, key):
    count = number(data, 20)
    if count > 256:
        raise ValueError('Excessive optional header count')
    for i in range(count):
        if number(data, 24 + i * 8) == key:
            return number(data, 28 + i * 8)
    raise ValueError(f'Missing optional header {key:08X}')


def aes(key, data):
    if len(data) % 16:
        raise ValueError('Unaligned AES-CBC input')
    decryptor = Cipher(algorithms.AES(key), modes.CBC(bytes(16))).decryptor()
    return decryptor.update(data) + decryptor.finalize()


def security(header):
    offset = number(header, 16)
    size = number(header, offset + 4)
    if not 1 <= size <= MAX_IMAGE:
        raise ValueError('Invalid image size')
    return offset, size


def version(raw):
    return f'{raw >> 28}.{(raw >> 24) & 15}.{(raw >> 8) & 65535}.{raw & 255}'


class Decoder:
    def __init__(self, library):
        self.lib = ctypes.CDLL(str(library.resolve()))
        self.decode = self.lib.bo2_lzx_decode
        self.decode.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                               ctypes.c_size_t, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_size_t]
        self.decode.restype = ctypes.c_int

    def lzx(self, source, length, window, reference=None):
        if not 1 <= length <= MAX_IMAGE:
            raise ValueError('Invalid LZX output size')
        output = ctypes.create_string_buffer(length)
        result = self.decode(source, len(source), output, length, window,
                             reference, len(reference) if reference is not None else 0)
        if result:
            raise ValueError(f'LZX decompression failed: {result}')
        return output.raw

    def delta(self, stream, dest, window):
        offset, patches = 0, 0
        while offset < len(stream):
            old, new, length, compressed = struct.unpack('>IIHH', take(stream, offset, 12))
            offset += 12
            if not (old or new or length or compressed):
                break
            take(dest, new, length)
            if compressed == 0:
                dest[new:new + length] = bytes(length)
            elif compressed == 1:
                dest[new:new + length] = take(dest, old, length)
            else:
                payload = take(stream, offset, compressed)
                reference = take(dest, old, length)
                dest[new:new + length] = self.lzx(payload, length, window, reference)
                offset += compressed
            patches += 1
        return patches


def blocks(data, descriptor):
    pass
    offset, count = 0, 0
    size = number(descriptor, 0)
    digest = take(descriptor, 4, 20)
    while size:
        if size < 24 or count > 65536:
            raise ValueError('Invalid compressed block chain')
        block = take(data, offset, size)
        if hashlib.sha1(block).digest() != digest:
            raise ValueError(f'Compressed block SHA-1 mismatch at block {count}')
        size, digest = number(block, 0), block[4:24]
        offset += len(block)
        count += 1
        yield block[24:]


def base_image(data, key, decoder):
    header_size = number(data, 8)
    header = bytearray(take(data, 0, header_size))
    sec, size = security(header)
    session = aes(key, take(header, sec + 0x150, 16))
    file_info = opt(header, 0x3FF)
    encryption, compression = struct.unpack('>HH', take(header, file_info + 4, 4))
    payload = take(data, header_size, len(data) - header_size)
    if encryption == 1:
        payload = aes(session, payload)
    elif encryption != 0:
        raise ValueError('Unsupported base encryption')
    if compression in (0, 1):
        image = bytearray(size)
        if compression == 0:
            take(image, 0, len(payload))
            image[:len(payload)] = payload
        else:
            info_bytes = number(header, file_info)
            if info_bytes < 8 or (info_bytes - 8) % 8:
                raise ValueError('Invalid basic compression descriptor')
            source, target = 0, 0
            for offset in range(file_info + 8, file_info + info_bytes, 8):
                data_bytes, zero_bytes = struct.unpack('>II', take(header, offset, 8))
                take(image, target, data_bytes + zero_bytes)
                image[target:target + data_bytes] = take(payload, source, data_bytes)
                source += data_bytes
                target += data_bytes + zero_bytes
        if image[:2] != b'MZ':
            raise ValueError('Decrypted base image has no DOS signature')
        return header, image, session, 0
    if compression != 2:
        raise ValueError('Unsupported base compression')
    window = number(header, file_info + 8)
    chunks, count = bytearray(), 0
    for block in blocks(payload, take(header, file_info + 12, 24)):
        cursor = 0
        while True:
            length = number(block, cursor, '>H')
            cursor += 2
            if not length:
                break
            chunks.extend(take(block, cursor, length))
            cursor += length
        count += 1
    image = bytearray(decoder.lzx(bytes(chunks), size, window))
    if image[:2] != b'MZ':
        raise ValueError('Decompressed image has no DOS signature')
    return header, image, session, count


def analyze(base, patch, decoder, root):
    for data in (base, patch):
        if data[:4] != b'XEX2':
            raise ValueError('Expected XEX2 inputs')
    source = (root / 'sdk/src/system/xex_module.cpp').read_text()
    key_text = re.search(r'xe_xex2_retail_key\[16\]\s*=\s*\{([^}]+)\}', source).group(1)
    retail_key = bytes(int(n, 16) for n in re.findall(r'0x[0-9A-Fa-f]+', key_text))
    header, image, original_session, base_blocks = base_image(base, retail_key, decoder)
    original_size = len(image)
    desc = opt(patch, 0x5FF)
    desc_size = number(patch, desc)
    descriptor = take(patch, desc, desc_size)
    sec, _ = security(header)
    if hashlib.sha1(take(header, sec + 8, 256)).digest() != descriptor[12:32]:
        raise ValueError('Delta source signature digest does not match base XEX')
    source_version = number(descriptor, 8)
    if number(header, opt(header, 0x40006) + 4) != source_version:
        raise ValueError('Delta source version does not match base XEX')
    target_header, header_old, header_len, header_new = struct.unpack('>IIII', descriptor[0x30:0x40])
    if not 24 <= target_header <= 16 * 1024 * 1024:
        raise ValueError('Invalid target header size')
    saved = take(header, header_old, header_len)
    if header_new + header_len > target_header:
        raise ValueError('Header copy exceeds target')
    header.extend(bytes(max(0, target_header - len(header))))
    if header_old:
        header[header_new:header_new + header_len] = saved
    header[target_header:] = bytes(max(0, len(header) - target_header))
    file_info = opt(patch, 0x3FF)
    window = number(patch, file_info + 8)
    compressed = number(descriptor, 0x4C + 10, '>H')
    header_patches = decoder.delta(take(descriptor, 0x4C, compressed + 12), header, window)
    del header[target_header:]
    sec, new_size = security(header)
    session = aes(retail_key, take(header, sec + 0x150, 16))
    if aes(session, descriptor[0x20:0x30]) != original_session:
        raise ValueError('Delta image source key does not match original')
    patch_session = aes(session, take(patch, number(patch, 16) + 0x150, 16))
    payload = patch[number(patch, 8):]
    encryption = number(patch, file_info + 4, '>H')
    if encryption == 1:
        payload = aes(patch_session, payload)
    elif encryption != 0:
        raise ValueError('Unsupported delta encryption')
    image.extend(bytes(max(0, new_size - len(image))))
    old, length, new = struct.unpack('>III', descriptor[0x40:0x4C])
    saved = take(image, old, length)
    take(image, new, length)
    if old:
        image[new:new + length] = saved
    if new + length < original_size:
        image[new + length:original_size] = bytes(original_size - new - length)
    delta_blocks, delta_patches = 0, 0
    for block in blocks(payload, take(patch, file_info + 12, 24)):
        delta_patches += decoder.delta(block, image, window)
        delta_blocks += 1
    del image[new_size:]
    pe = number(image, 0x3C, '<I')
    if take(image, pe, 4) != b'PE\x00\x00':
        raise ValueError('Patched image has no PE signature')
    section_count = number(image, pe + 6, '<H')
    section_table = pe + 24 + number(image, pe + 20, '<H')
    image_base = number(image, pe + 24 + 28, '<I')
    sections = []
    for i in range(section_count):
        s = take(image, section_table + i * 40, 40)
        sections.append({'name': s[:8].rstrip(b'\0').decode('ascii'),
                         'address': f'{image_base + number(s,12,"<I"):08X}',
                         'bytes': number(s,8,'<I')})
    digest = hashlib.sha256(image).hexdigest()
    report = {'analyzer': 'independent SDK-algorithm decoder; no Runtime/kernel initialization',
              'base_sha256': hashlib.sha256(base).hexdigest(),
              'patch_sha256': hashlib.sha256(patch).hexdigest(),
              'base_blocks_sha1_verified': base_blocks,
              'delta_blocks_sha1_verified': delta_blocks,
              'delta_source_signature_verified': True, 'delta_source_key_verified': True,
              'header_delta_records': header_patches, 'image_delta_records': delta_patches,
              'source_version': version(source_version), 'target_version': version(number(descriptor,4)),
              'final_version': version(number(header,opt(header,0x40006)+4)),
              'image_base': f'{image_base:08X}', 'image_bytes': len(image),
              'image_sha256_before_import_resolution': digest,
              'expected_image_sha256': EXPECTED,
              'matches_reference_before_import_resolution': digest == EXPECTED,
              'sections': sections}
    return header, image, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', required=True, type=Path)
    parser.add_argument('--lzx-library', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    if not args.out.resolve().is_relative_to(root / 'work'):
        parser.error('Keep private derived files under the ignored repository work/ directory')
    header, image, report = analyze((args.game / 'default_mp.xex').read_bytes(),
                                   (args.game / 'default_mp.xexp').read_bytes(),
                                   Decoder(args.lzx_library), root)
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / 'default_mp.image.bin').write_bytes(image)
    (args.out / 'default_mp.patched-header.bin').write_bytes(header)
    (args.out / 'analysis.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
