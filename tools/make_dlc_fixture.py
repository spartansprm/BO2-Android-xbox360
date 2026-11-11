#!/usr/bin/env python3

import struct
import sys
from pathlib import Path

out=Path(sys.argv[1]);out.parent.mkdir(parents=True,exist_ok=True)
b=bytearray(0x10000);b[:4]=b'LIVE'
for offset,value in [(0x340,0x971A),(0x344,2),(0x360,0x415608C3),(0x395,5)]:struct.pack_into('>I',b,offset,value)
b[0x379]=0x24;b[0x37B]=1;b[0x37C]=1
b[0x411:0x411+len('Authored DLC')*2]='Authored DLC'.encode('utf-16-be')
struct.pack_into('>II',b,0x22C+8,0xA,1)
struct.pack_into('>II',b,0x23C+8,0x4,0)
for i,next_block in enumerate([0xFFFFFF,0xFFFFFF,3,4,0xFFFFFF]):struct.pack_into('>I',b,0xA000+i*24+20,next_block)
def entry(slot,name,block,size,blocks):
 at=0xB000+slot*64;encoded=name.encode();b[at:at+len(encoded)]=encoded;b[at+0x28]=len(encoded)
 b[at+0x2C:at+0x2F]=blocks.to_bytes(3,'little');b[at+0x2F:at+0x32]=block.to_bytes(3,'little');b[at+0x32:at+0x34]=b'\xff\xff';struct.pack_into('>I',b,at+0x34,size)
entry(0,'dlczm0.id',1,4,1);entry(1,'zm_test.ff',2,9000,3)
b[0xC000:0xC004]=b'TEST';b[0xD000:0xD000+9000]=bytes((i*17+9)&255 for i in range(9000));out.write_bytes(b)
