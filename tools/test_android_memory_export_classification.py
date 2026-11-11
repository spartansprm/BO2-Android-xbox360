#!/usr/bin/env python3

import struct
import unittest
from compile_android_shader_capture import exports_guest_memory

def blob(words,vertex=True):
    data=bytearray(64+len(words)*4)
    struct.pack_into('>III',data,0,0x102A1100|vertex,64,len(words)*4)
    struct.pack_into('>I',data,24,36);struct.pack_into('>II',data,36,0,len(words)*4)
    struct.pack_into('>'+str(len(words))+'I',data,64,*words);return data

class MemoryExports(unittest.TestCase):
    def test_first_and_second_cf(self):
        self.assertTrue(exports_guest_memory(blob([0,0xC600,0])))
        self.assertTrue(exports_guest_memory(blob([0,0,0xC6000000])))
    def test_raster_alloc_and_pixel(self):
        for value in [0,0xC000,0xC200,0xC400]:self.assertFalse(exports_guest_memory(blob([0,value,0])))
        self.assertFalse(exports_guest_memory(blob([0,0xC600,0],False)))
    def test_stops_at_end(self):
        for end in [0x2000,0x4000]:self.assertFalse(exports_guest_memory(blob([0,end,0,0,0xC600,0])))
    def test_scan_bound_and_tail(self):
        self.assertFalse(exports_guest_memory(blob([0]*192+[0,0xC600,0])))
        self.assertFalse(exports_guest_memory(blob([0,0xC600])))
    def test_bounds(self):
        data=blob([0,0xC600,0]);struct.pack_into('>I',data,40,16)
        with self.assertRaisesRegex(ValueError,'Microcode'):exports_guest_memory(data)

if __name__=='__main__':unittest.main()
