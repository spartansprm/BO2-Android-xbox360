#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only

import ctypes
import ctypes.util
import struct

_sodium=None
_lzx=None

def initialize(library):
    global _sodium,_lzx
    _sodium=ctypes.CDLL(ctypes.util.find_library('sodium'))
    _sodium.sodium_init.restype=ctypes.c_int
    if _sodium.sodium_init()<0:raise RuntimeError('Cannot initialize libsodium')
    _sodium.crypto_stream_salsa20.argtypes=[ctypes.c_void_p,ctypes.c_ulonglong,ctypes.c_char_p,ctypes.c_char_p]
    _sodium.crypto_stream_salsa20.restype=ctypes.c_int
    _lzx=ctypes.CDLL(str(library))
    _lzx.bo2_lzx_decode.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_size_t,
                                ctypes.c_uint32,ctypes.c_void_p,ctypes.c_size_t]
    _lzx.bo2_lzx_decode.restype=ctypes.c_int

def salsa20_keystream(key,iv,nbytes):
    if len(key)!=32 or len(iv)!=8 or not 0<=nbytes<=32768:raise ValueError('Invalid Salsa20 chunk')
    if not nbytes:return b''
    out=ctypes.create_string_buffer(nbytes)
    if _sodium.crypto_stream_salsa20(out,nbytes,iv,key):raise ValueError('Salsa20 failed')
    return out.raw

def xmem_decompress(chunk):
    compressed=[];total=0;i=0
    while i<len(chunk):
        hi=chunk[i];i+=1
        if hi==255:
            if len(chunk)-i<4:raise ValueError('Short XMem frame')
            dst,src=struct.unpack_from('>HH',chunk,i);i+=4;suffix=5
        else:
            if len(chunk)-i<1:raise ValueError('Short XMem frame')
            dst=32768;src=(hi<<8)|chunk[i];i+=1;suffix=0
        if not src or not dst or i+src+suffix>len(chunk):raise ValueError('Invalid XMem frame bounds')
        total+=dst
        if total>128*1024*1024:raise ValueError('XMem expansion exceeds bound')
        compressed.append(chunk[i:i+src]);i+=src+suffix
    if not total:raise ValueError('Empty XMem chunk')
    source=b''.join(compressed);out=ctypes.create_string_buffer(total)
    result=_lzx.bo2_lzx_decode(source,len(source),out,total,1<<17,None,0)
    if result:raise ValueError(f'Native LZX failed: {result}')
    return out.raw

def decompress_job(chunk):
    try:return xmem_decompress(chunk)
    except Exception as e:return e
