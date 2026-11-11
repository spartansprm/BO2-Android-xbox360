#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only


import argparse
from concurrent.futures import ThreadPoolExecutor,as_completed
import csv
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import zipfile
from check_ubo_spirv import inspect_spirv,require_ubo_only
from validate_android_shader_capture import validate_container

ROOT=Path(__file__).resolve().parents[1]
def sha(data):return hashlib.sha256(data).hexdigest()

def exports_guest_memory(blob):
    pass


    if validate_container(blob)!='v':return False
    virtual,physical=struct.unpack_from('>II',blob,4)
    header=struct.unpack_from('>I',blob,24)[0]
    if header+8>virtual:raise ValueError('Shader header outside virtual data')
    start,size=struct.unpack_from('>II',blob,header)
    if not size or start%4 or size%4 or start+size>physical:raise ValueError('Microcode outside physical data')
    words=struct.unpack_from('>'+str(size//4)+'I',blob,virtual+start)
    for i in range(0,min(len(words)-2,3*64),3):
        end=False
        for high in [words[i+1]&0xffff,words[i+2]>>16]:
            opcode=(high>>12)&15
            if opcode==12 and (high>>9)&3==3:return True
            end|=opcode in (2,4)
        if end:break
    return False

def run(command,log):
    with log.open('wb') as output:subprocess.run([str(x) for x in command],stdout=output,stderr=subprocess.STDOUT,check=True)

def process(a):
    out=a.out.resolve();staged=a.staged.resolve()
    if out.is_relative_to(ROOT) or out.exists():raise ValueError('Output must be NEW outside the public source tree')
    source=json.loads((staged/'capture-validation.json').read_text())
    paths=sorted((staged/'containers').glob('*.bin'))
    if len(paths)!=source['merged_unique'] or not 1<=len(paths)<=65536:raise ValueError('Staged container count mismatch')
    if sum(p.stat().st_size for p in paths)>512*1024*1024:raise ValueError('Staged containers exceed budget')
    eligible=[];cpu=[]
    for p in paths:
        blob=p.read_bytes();kind=validate_container(blob)
        expected=kind+'s_'+hashlib.sha1(blob).hexdigest()[:16]+'.bin'
        if p.name!=expected:raise ValueError('Noncanonical/corrupt staged container: '+p.name)
        (cpu if exports_guest_memory(blob) else eligible).append(p)
    out.mkdir(parents=True);containers=out/'containers';containers.mkdir()
    for p in eligible:shutil.copyfile(p,containers/p.name)
    report={'status':'working','profile':'ubo-only','private_game_resources':True,
            'scope':'boot library plus captured registered shaders; physical visual correction pending',
            'capture':source,'merged_containers':len(paths),'unique_containers':len(eligible),
            'cpu_memory_export_containers':[p.name for p in cpu],
            'common_header_sha256':sha((ROOT/'tools/xenosrecomp/translator/shader_common.h').read_bytes()),
            'dxc_sha256':sha(a.dxc.read_bytes()),'spirv_val_sha256':sha(a.spirv_val.read_bytes())}
    try:
        hlsl=a.translated.resolve() if a.translated else out/'hlsl'
        if not a.translated:
            run([a.tools/'bo2zm_hlsl',containers,hlsl,ROOT/'tools/xenosrecomp/translator/shader_common.h'],out/'translate.log')
        rows=list(csv.DictReader((hlsl/'translate.tsv').open(),delimiter='\t'))
        by_name={row['name']:row for row in rows}
        if len(by_name)!=len(rows):raise ValueError('Duplicate translation rows')
        for p in eligible:
            if by_name.get(p.stem,{}).get('status')!='ok':raise ValueError('Ordinary shader translation incomplete: '+p.name)
        allowed={p.stem for p in paths}
        if set(by_name)-allowed:raise ValueError('Unexpected translation rows')
        spirv=out/'spirv';spirv.mkdir();jobs={};modules=[]
        for p in eligible:
            vertex=p.name.startswith('vs_');name=('v_' if vertex else 'p_')+p.stem
            text=hlsl/(name+'.hlsl');key=(vertex,sha(text.read_bytes()))
            modules.append((p,text,spirv/(name+'.spv'),key));jobs.setdefault(key,(text,spirv/(name+'.spv')))
        capabilities=set();total_bytes=0
        def compile_one(item):
            (vertex,_),(text,target)=item
            run([a.dxc,'-spirv','-T','vs_6_6' if vertex else 'ps_6_6','-E','main','-HV','2021',
                 '-fspv-target-env=vulkan1.2','-fvk-use-dx-layout','-DBO2ZM_UBO_ONLY=1',
                 *(['-fvk-invert-y'] if vertex else []),'-Fo',target,text],target.with_suffix('.dxc.log'))
            run([a.spirv_val,'--target-env','vulkan1.2','--scalar-block-layout',target],target.with_suffix('.validation.log'))
            info=inspect_spirv(target.read_bytes());require_ubo_only(info)
            return info['capabilities'],target.stat().st_size
        with ThreadPoolExecutor(max_workers=a.jobs) as pool:
            futures=[pool.submit(compile_one,item) for item in jobs.items()]
            for count,f in enumerate(as_completed(futures),1):
                caps,n=f.result();capabilities.update(caps);total_bytes+=n
                if count%100==0 or count==len(jobs):print(f'Compiled and validated {count}/{len(jobs)} distinct HLSL inputs',flush=True)
        for _,_,target,key in modules:
            reference=jobs[key][1]
            if target!=reference:shutil.copyfile(reference,target)
        pack=out/'bo2zm_shaders.spvpack'
        run([a.tools/'bo2zm_pack',containers,spirv,pack],out/'pack.log')
        run([a.tools/'bo2zm_pack','--index',pack],out/'index.log')


        run([a.tools/'bo2zm_pack','--coverage',pack],out/'coverage.tsv')
        coverage=list(csv.DictReader((out/'coverage.tsv').open(),delimiter='\t'))
        if len(coverage)!=len(modules) or sum(r['stage']=='vs' for r in coverage)!=sum(p.name.startswith('vs_') for p in eligible):
            raise ValueError('Packed shader count/stages differ from compiled inputs')
        report.update(status='ready',validated_modules=len(modules),distinct_compiler_inputs=len(jobs),
            stages={'vertex':sum(p.name.startswith('vs_') for p in eligible),'pixel':sum(p.name.startswith('ps_') for p in eligible)},
            capabilities=sorted(capabilities),pack_bytes=pack.stat().st_size,pack_sha256=sha(pack.read_bytes()),
            index_sha256=sha(pack.with_suffix('.spvidx').read_bytes()))
        if a.compact:
            archive=out/'validated-spirv.zip'
            with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
                for _,_,target,_ in modules:z.write(target,target.name)
            with zipfile.ZipFile(archive) as z:
                for _,_,target,_ in modules:
                    if sha(z.read(target.name))!=sha(target.read_bytes()):raise ValueError('SPIR-V cache readback differs')
                    target.unlink()
            report['spirv_cache_sha256']=sha(archive.read_bytes())
    except Exception as e:
        report.update(status='error',error=str(e));raise
    finally:(out/'shader-build-results.json').write_text(json.dumps(report,indent=2)+'\n')
    print(f"Ready: {len(eligible)} raster shaders; {len(cpu)} guest-memory exports stay on CPU; {pack.stat().st_size} pack bytes",flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--staged',required=True,type=Path)
    p.add_argument('--out',required=True,type=Path);p.add_argument('--tools',required=True,type=Path)
    p.add_argument('--translated',type=Path);p.add_argument('--dxc',required=True,type=Path)
    p.add_argument('--spirv-val',required=True,type=Path);p.add_argument('--jobs',type=int,default=3)
    p.add_argument('--compact',action='store_true');a=p.parse_args()
    if not 1<=a.jobs<=8:p.error('Choose 1..8 compiler workers')
    process(a)
