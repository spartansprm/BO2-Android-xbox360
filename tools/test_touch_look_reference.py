#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only


import argparse, hashlib, json, random, struct, subprocess, tempfile, zipfile
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X29, UC_ARM64_REG_X30, UC_ARM64_REG_SP, UC_ARM64_REG_S0, UC_ARM64_REG_S1
APK_SHA256 = '01e85c2000d96cc00401f40600f8af81cb27caf2caf86d764360e8a0015435d7'
STOP, STACK, OUTPUT = 0x500000, 0x410000, 0x420000
OWNER, LAST, ACC_X, ACC_Y, ASPECT = 0x2B5F00, 0x2B5F8C, 0x2B5F0C, 0x2B5F08, 0x2A0BD0
def fp(value): return struct.unpack('<f', struct.pack('<f', value))[0]
def bits(value): return struct.pack('<f', value)
class NativeArithmetic:
    def __init__(self, aspect):
        self.cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.cpu.mem_map(0, 0x300000)
        self.cpu.mem_map(0x400000, 0x30000)
        self.cpu.mem_map(STOP, 0x1000)
        with LIBRARY.open('rb') as stream:
            elf = ELFFile(stream)
            for segment in elf.iter_segments():
                if segment['p_type'] == 'PT_LOAD':
                    self.cpu.mem_write(segment['p_vaddr'], segment.data())
        self.cpu.mem_write(ASPECT, bits(aspect))
        self.cpu.mem_write(ACC_X, bits(0))
        self.cpu.mem_write(ACC_Y, bits(0))
        self.cpu.mem_write(OWNER, struct.pack('<Q', 0))

    def call(self, address, owner=0, x=0.0, y=0.0):
        self.cpu.reg_write(UC_ARM64_REG_SP, STACK)
        self.cpu.reg_write(UC_ARM64_REG_X30, STOP)
        self.cpu.reg_write(UC_ARM64_REG_X0, owner)
        self.cpu.reg_write(UC_ARM64_REG_S0, struct.unpack('<I', bits(x))[0])
        self.cpu.reg_write(UC_ARM64_REG_S1, struct.unpack('<I', bits(y))[0])
        self.cpu.emu_start(address, STOP, count=1000)

    def begin(self, owner, position):
        self.call(0x177840, owner, *position)

    def move(self, owner, position):
        self.call(0x177ADC, owner, *position)

    def relative(self, dx, dy):
        self.call(0x176C7C, 0, dx, dy)

    def consume(self):


        frame = STACK + 0x100
        self.cpu.reg_write(UC_ARM64_REG_X29, frame)
        self.cpu.mem_write(frame - 0x40, struct.pack('<Q', OUTPUT))
        self.cpu.emu_start(0x17A4AC, 0x17A4D8, count=100)
        return struct.unpack('<ff', self.cpu.mem_read(OUTPUT + 0x20, 8))

    def pending(self):
        return (struct.unpack('<f', self.cpu.mem_read(ACC_X, 4))[0],
                struct.unpack('<f', self.cpu.mem_read(ACC_Y, 4))[0])


class Quantizer:
 def __init__(self):
  self.cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.cpu.mem_map(0x1100000,0x100000);self.cpu.mem_map(0x4f74000,0x1000);self.cpu.mem_map(0x6000000,0x2000)
  with GUEST.open('rb') as f:
   e=ELFFile(f);s=e.get_section_by_name('.text');addr=0x11aa9bc
   self.cpu.mem_write(addr,s.data()[addr-s['sh_addr']:0x11aaa4c-s['sh_addr']])
  self.sp=0x6001000;self.cpu.reg_write(UC_ARM64_REG_SP,self.sp)
 def consume(self,x,y):
  self.cpu.mem_write(self.sp+0x48,struct.pack('<ff',x,y));self.cpu.emu_start(0x11aa9bc,0x11aaa4c,count=100)
  return struct.unpack('<ii',self.cpu.mem_read(self.sp+0x58,4)+self.cpu.mem_read(self.sp+0x54,4))

def run():
 rng=random.Random(520);events=[];expected=[];cases=[]
 for w,h in ((800,360),(1280,720),(1920,1080),(2340,1080),(2400,1080),(2560,1440)):
  native=NativeArithmetic(fp(fp(w/h)*.5625));q=Quantizer();owner=65537;x,y=fp(w*.4),fp(h*.5)
  def norm(x,y):return fp(x/w),fp(y/h)
  def point(x,y):nx,ny=norm(x,y);return fp(nx*1280),fp(ny*720)
  nx,ny=norm(x,y);events+=['r',f'e 0 3 {nx:.17g} {ny:.17g} {w} {h}'];native.begin(owner,point(x,y))
  count=0
  for step in range(600):
   x=fp(x+rng.uniform(-60,60));y=fp(y+rng.uniform(-45,45));nx,ny=norm(x,y)
   wrong=step%13==0;events.append(f'e 1 {4 if wrong else 3} {nx:.17g} {ny:.17g} {w} {h}')
   native.move(owner+int(wrong),point(x,y))
   if step%4==0:
    delta=native.consume();expected.append(q.consume(*delta));events.append('c');count+=1
    expected.append(q.consume(0,0));events.append('c');count+=1

  x=fp(x+21.375);y=fp(y-8.75);nx,ny=norm(x,y)
  native.move(owner,point(x,y));events.append(f'e 2 3 {nx:.17g} {ny:.17g} {w} {h}')
  expected.append(q.consume(*native.consume()));events.append('c');count+=1
  cases.append({'width':w,'height':h,'native_count_pairs':count})
 result=subprocess.run([str(RUNNER)],input='\n'.join(events)+'\n',text=True,capture_output=True,check=True)
 actual=[tuple(map(int,line.split())) for line in result.stdout.splitlines()]
 assert len(actual)==len(expected),(len(actual),len(expected))
 for i,(a,b) in enumerate(zip(actual,expected)):
  assert a==b,(i,a,b)
 report={'scope':'Implemented C++ gesture accumulator and count conversion versus arithmetic instructions from the official host and guest binaries. Does not run Android or gameplay.', 'reference_apk_sha256':APK_SHA256,'reference_guest_sha256':hashlib.sha256(GUEST.read_bytes()).hexdigest(),'matching_count_pairs':len(actual),'cases':cases}
 print(json.dumps(report,indent=2))

if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('apk',type=Path);args=parser.parse_args()
    if hashlib.sha256(args.apk.read_bytes()).hexdigest()!=APK_SHA256:parser.error('The APK differs from the pinned official reference')
    with tempfile.TemporaryDirectory(prefix='bo2-touch-reference-') as folder:
        folder=Path(folder);LIBRARY=folder/'host.so';GUEST=folder/'guest.elf';RUNNER=folder/'touch-stream'
        with zipfile.ZipFile(args.apk) as archive:
            LIBRARY.write_bytes(archive.read('lib/arm64-v8a/libt6host.so'));GUEST.write_bytes(archive.read('assets/guest/t6mp_guest.elf'))
        source=folder/'stream.cpp';source.write_text('#include "touch_look.h"\n#include <iostream>\nint main(){bo2zm::android::TouchLook look;char c;while(std::cin>>c){if(c==\'e\'){int a,p;float x,y,w,h;std::cin>>a>>p>>x>>y>>w>>h;look.Touch(a,p,x,y,w,h);}else if(c==\'r\')look.Cancel();else if(c==\'c\'){auto v=look.Consume();std::cout<<v.x<<\' \'<<v.y<<\'\\n\';}}}\n')
        subprocess.run(['g++','-std=c++20','-O2','-ffp-contract=off','-pthread','-I',str(Path(__file__).resolve().parents[1]/'android/game'),str(source),'-o',str(RUNNER)],check=True)
        run()
