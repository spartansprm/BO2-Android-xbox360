#!/usr/bin/env python3


import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct

from compile_android_shader_capture import exports_guest_memory
from compile_android_zone_library import XXH3


def classify(state_path, containers, previous_pack, output):
    state=json.loads(state_path.read_text())
    if (state['status']!='extracted' or not state['transport_complete'] or state['summary']['failed_zones']
            or len(state['files'])!=state['expected_files'] or len(state['batches'])!=state['expected_batches']):
        raise ValueError('Extraction is incomplete')
    if output.exists():raise ValueError('Expected a new classification output')
    previous={};missing=0;xx=XXH3()
    with previous_pack.open('rb') as stream:
        magic,version,count,body_hash=struct.unpack('<8sIIQ',stream.read(24))
        if magic!=b'BZMSPV\0\0' or version!=1 or not 1<=count<=65536:raise ValueError('Unsupported previous pack')
        for index in range(count):
            header=stream.read(16)
            if len(header)!=16:raise ValueError('Truncated previous pack')
            original,words,expected=struct.unpack('<IIQ',header)
            if not 24<=original<=65536 or not 5<=words<=1024*1024:raise ValueError('Invalid previous pack bounds')
            blob=stream.read(original);offset=stream.tell();code=stream.read(words*4)
            if len(blob)!=original or len(code)!=words*4 or xx.one(blob)!=expected:raise ValueError('Previous pack entry integrity failed')
            xx.update(header);xx.update(blob);xx.update(code)
            name=('vs' if struct.unpack_from('>I',blob)[0]&1 else 'ps')+'_'+hashlib.sha1(blob).hexdigest()[:16]+'.bin'
            if name in previous:raise ValueError('Repeated previous container')
            previous[name]={'entry':index,'spirv_offset':offset,'spirv_bytes':len(code),
                            'spirv_sha256':hashlib.sha256(code).hexdigest(),'container_bytes':original}
            target=containers/name
            missing+=name not in state['containers']
            if not target.exists():target.write_bytes(blob)
            elif target.read_bytes()!=blob:raise ValueError('Previous/source container filename collision')
        if stream.read(1) or xx.value()!=body_hash:raise ValueError('Previous pack body integrity failed')
    rejected=[]
    for zone in state['files'].values():
        for item in zone['unmatched']:
            if item['virtual']<=65535 or item['physical']<=65535:
                raise ValueError('Unmatched candidate needs review: '+str(zone['id'])+' '+str(item))
            rejected.append({'zone_id':zone['id'],'zone':zone['path'],'offset':item['container'],
                             'reason':'Signature occurs in data; virtual and physical lengths exceed XDK record fields',
                             'virtual':item['virtual'],'physical':item['physical']})
    rows=[];counts=Counter()
    for name in sorted(set(state['containers'])|set(previous)):
        blob=(containers/name).read_bytes();offline=state['containers'].get(name)
        zones=offline['zones'] if offline else []
        modes=set().union(*(set(state['files'][str(z)]['modes']) for z in zones)) if zones else {'mp','zm'}
        if name in previous:modes.update(('mp','zm'))
        if offline and (len(blob)!=offline['bytes'] or hashlib.sha256(blob).hexdigest()!=offline['sha256']):
            raise ValueError('Extracted container integrity mismatch')
        cpu=exports_guest_memory(blob);selected=bool(modes&{'mp','zm'}) or name in previous
        counts[str(('zm_mp' if selected else 'campaign_only','cpu' if cpu else 'raster'))]+=1
        counts[str(('stage',name[:2]))]+=1
        row={'file':name,'modes':sorted(modes),'cpu_memory_export':cpu,'in_previous_library':name in previous,
             'bytes':len(blob),'sha256':hashlib.sha256(blob).hexdigest(),'zones':zones}
        if name in previous:row['previous_spirv']=previous[name]
        rows.append(row)
    report={'status':'extracted_and_classified','transport_complete':True,'source_run_id':state['run_id'],
            'source_files':len(state['files']),'source_bytes':state['validated_bytes'],'batches':len(state['batches']),
            'offline_unique':len(state['containers']),'previous_library_shaders':len(previous),'previous_not_found_offline':missing,
            'merged_containers':len(rows),'rejected_non_container_signatures':rejected,'counts':dict(counts),
            'containers':rows,'codec_checks':state['codec_checks']}
    output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k not in ('containers','rejected_non_container_signatures')}))


if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--state',type=Path,required=True);ap.add_argument('--containers',type=Path,required=True)
    ap.add_argument('--previous-pack',type=Path,required=True);ap.add_argument('--out',type=Path,required=True)
    a=ap.parse_args();classify(a.state,a.containers,a.previous_pack,a.out)
