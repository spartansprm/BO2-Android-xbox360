#!/usr/bin/env python3

from pathlib import Path
import argparse,struct

def write(path, offset, data):
    with path.open('r+b') as f: f.seek(offset); f.write(data)

def location(block):
    actual=block+2; within=actual%0x14388
    return actual//0x14388,0x2000+within*2048+(within//408)*4096

def main():
    ap=argparse.ArgumentParser();ap.add_argument('out',type=Path);a=ap.parse_args();a.out.mkdir(parents=True,exist_ok=True)
    header=bytearray(0xC000);header[:4]=b'LIVE'
    def be(at,value):struct.pack_into('>I',header,at,value)
    be(0x340,0xB000);be(0x344,0x7000);be(0x354,0x457AD131);be(0x360,0x415608C3)
    be(0x39D,2);be(0x3A9,1);header[0x379]=0x24;header[0x391]=0x40
    (a.out/'game.header').write_bytes(header)
    paths=[a.out/'Data0000',a.out/'Data0001']
    for p,length in zip(paths,[0xA290000,0x10000]):
        with p.open('wb') as f:f.truncate(length)
    def virtual(block,data):
        for at in range(0,len(data),2048):
            frag,off=location(block+at//2048);write(paths[frag],off,data[at:at+2048])
    root=405;block=0x14388-4
    media=bytearray(36);media[:20]=b'MICROSOFT*XBOX*MEDIA';struct.pack_into('<II',media,20,root,4096);write(paths[0],0x2000,media)
    data=bytearray((i*17+9)&255 for i in range(9000));data[:4]=b'XEX2'
    directory=bytearray(4096);struct.pack_into('<H',directory,2,508);directory[13]=5;directory[14:19]=b'other'
    at=508*4;struct.pack_into('<II',directory,at+4,block,len(data));directory[at+13]=14;directory[at+14:at+28]=b'default_mp.xex'

    virtual(root,directory);virtual(block,data)
    patch=bytearray(0xE000);patch[:4]=b'LIVE'
    struct.pack_into('>I',patch,0x340,0x971A);struct.pack_into('>I',patch,0x344,0xB0000)
    struct.pack_into('>I',patch,0x354,0x457AD131);struct.pack_into('>I',patch,0x360,0x415608C3)
    patch[0x379]=0x24;patch[0x37B]=1;patch[0x37C]=1;struct.pack_into('>I',patch,0x395,4)

    patch.extend(bytes(0x1000));at=0xB000;name=b'default_mp.xexp';patch[at:at+len(name)]=name;patch[at+0x28]=len(name)
    patch[at+0x2C:at+0x2F]=bytes([3,0,0]);patch[at+0x2F:at+0x32]=bytes([1,0,0]);patch[at+0x32:at+0x34]=b'\xff\xff'
    struct.pack_into('>I',patch,at+0x34,len(data));patch[0xC000:0xC000+len(data)]=data
    for i in range(4):struct.pack_into('>I',patch,0xA000+i*24+20,i+1 if 0<i<3 else 0xFFFFFF)
    (a.out/'update.stfs').write_bytes(patch)
    bad=bytearray(patch);bad[0xB032:0xB034]=b'\x00\x01'
    (a.out/'bad-parent.stfs').write_bytes(bad)
    bad=bytearray(patch);bad[0xA000+24+20:0xA000+24+24]=bytes([0,0,0,1])
    (a.out/'cyclic-chain.stfs').write_bytes(bad)
    (a.out/'truncated.stfs').write_bytes(patch[:0xC010])
    cycle=a.out/'cyclic-svod.data'
    with cycle.open('wb') as f:f.truncate(0xA290000)
    write(cycle,0x2000,media)
    struct.pack_into('<H',directory,508*4,508)
    for at in range(0,len(directory),2048):
        _,off=location(root+at//2048);write(cycle,off,directory[at:at+2048])
    print(a.out)
if __name__=='__main__':main()
