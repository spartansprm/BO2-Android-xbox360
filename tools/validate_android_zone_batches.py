#!/usr/bin/env python3


import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import uuid
import zipfile

ROOT=Path(__file__).resolve().parents[1]
MIB=1024*1024
BASE='7bd174dc01cf55ac378acad44aa29a447357bd3617214155b7aa7fa64c9a912d'
PATCHES={'7f4ee5e4b5ccfe71786ddc073a0363558e89634bb26c2929335aeb7f883c9499',
         '5d5780970f6e639b17c0570bae0e52a13bd1038e23c969428aef9d0c7d78681b'}

def need(ok,message):
    if not ok:raise ValueError(message)

def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        while b:=f.read(MIB):h.update(b)
    return h.hexdigest()

def valid_hash(value):return isinstance(value,str) and re.fullmatch('[0-9a-f]{64}',value)

def validate(index_path,out):
    index_path=index_path.resolve();out=out.resolve()
    need(not out.exists() and not out.is_relative_to(ROOT),'Output must be NEW and outside the public source tree')
    need(index_path.stat().st_size<=8*MIB,'Export index exceeds limit')
    index=json.loads(index_path.read_bytes())
    need(index.get('schema')==1 and index.get('kind')=='bo2-fastfile-export' and
         index.get('private_game_resources') is True and index.get('complete') is True,'Not a complete private zone export')
    run=index.get('run_id');need(str(uuid.UUID(run))==run,'Invalid export run ID')
    inventory=index['inventory'];need(inventory.get('scope')=='all-fastfiles-in-selected-base-update-dlc','Unexpected export scope')
    checks=inventory['source_checks'];need(checks.get('base_xex_sha256')==BASE and checks.get('patch_sha256') in PATCHES,'Unverified game/TU18 source pair')
    zones=inventory['zones'];files=index['files'];batches=index['batches']
    need(1<=len(zones)<=8192 and len(files)==len(zones) and 1<=len(batches)<=1024,'Invalid catalogue/batch counts')
    total=0;paths=[];identities=set()
    for i,(z,f) in enumerate(zip(zones,files)):
        need(type(z.get('id')) is int and z.get('id')==i and type(f.get('id')) is int and f.get('id')==i,'Noncanonical zone IDs')
        size=z.get('bytes');need(type(size) is int and 1<=size<=1024*MIB,'Invalid zone size')
        need(all(f.get(k)==z.get(k) for k in ('origin','path','bytes')) and valid_hash(f.get('sha256')),'File/inventory mismatch')
        origin=z.get('origin');path=z.get('path')
        need(origin in ('game','update') or isinstance(origin,str) and re.fullmatch(r'dlc/[^/\\\x00]{1,42}',origin),'Invalid package origin')
        need(isinstance(path,str) and len(path)<=1024,'Invalid source path')
        name=path.replace('\\','/').rsplit('/',1)[-1]
        need(re.fullmatch(r'[A-Za-z0-9_.-]{1,180}\.ff',name,re.I),'Invalid fastfile basename')
        identity=(origin,path);need(identity not in identities,'Duplicate source zone');identities.add(identity)
        paths.append(Path('zones')/f'z{i:04d}'/name);total+=size
    need(total<=16*1024*MIB and index.get('total_bytes')==total,'Invalid total size')
    received=[0]*len(zones);parts_seen=set();part_count=0
    out.mkdir(parents=True)
    try:
        for number,batch in enumerate(batches,1):
            name=batch.get('file');need(name==f'bo2-zonas-{number:04d}.zip','Noncanonical batch name/order')
            archive=index_path.parent/name
            need(archive.is_file() and not archive.is_symlink(),'Missing batch: '+str(name))
            need(1<=archive.stat().st_size<=32*MIB and archive.stat().st_size==batch.get('bytes') and
                 digest(archive)==batch.get('sha256'),'Batch size/hash mismatch: '+name)
            with zipfile.ZipFile(archive) as z:
                infos=z.infolist();names=[x.filename for x in infos]
                need(len(names)==len(set(names)) and len(infos)<=8193,'Duplicate/excess ZIP entries')
                need(all(not x.is_dir() and not x.flag_bits&1 and (x.external_attr>>16)&0o170000!=0o120000 for x in infos),'Unsupported ZIP entry')
                need(sum(x.file_size for x in infos)<=32*MIB,'ZIP expansion exceeds budget')
                need(z.getinfo('zone_batch_manifest.json').file_size<=4*MIB,'Batch manifest exceeds budget')
                m=json.loads(z.read('zone_batch_manifest.json'))
                need(m.get('schema')==1 and m.get('kind')=='bo2-fastfile-batch' and m.get('private_game_resources') is True and
                     m.get('run_id')==run and m.get('app_version')==index.get('app_version') and m.get('batch')==number and
                     m.get('inventory')==inventory,'Batch provenance/inventory mismatch')
                listed={'zone_batch_manifest.json'}
                for p in m['parts']:
                    entry=p.get('entry');i=p.get('zone_id');offset=p.get('offset');size=p.get('bytes')
                    need(type(i) is int and 0<=i<len(zones) and re.fullmatch(r'zones/z\d{4}-p\d{4}\.bin',entry or ''),'Invalid part name/ID')
                    need(entry.startswith(f'zones/z{i:04d}-') and entry not in listed and entry not in parts_seen,'Duplicate/misassigned part')
                    need(type(size) is int and 1<=size<=24*MIB and type(offset) is int and offset==received[i] and
                         size<=zones[i]['bytes']-received[i] and valid_hash(p.get('sha256')),'Part gap, overlap or size/hash error')
                    need(z.getinfo(entry).file_size==size,'Part ZIP size mismatch')
                    target=out/paths[i];target.parent.mkdir(parents=True,exist_ok=True);h=hashlib.sha256();got=0
                    with z.open(entry) as src,target.open('wb' if offset==0 else 'ab') as dst:
                        while data:=src.read(128*1024):h.update(data);dst.write(data);got+=len(data)
                    need(got==size and h.hexdigest()==p['sha256'],'Part content hash mismatch')
                    received[i]+=size;listed.add(entry);parts_seen.add(entry);part_count+=1
                need(set(names)==listed,'Unlisted ZIP entries')
        for i,f in enumerate(files):
            need(received[i]==f['bytes'] and digest(out/paths[i])==f['sha256'],'Missing or corrupt complete zone')
        report={'status':'validated','private_game_resources':True,'scope':inventory['scope'],'run_id':run,
                'fastfiles':len(zones),'batches':len(batches),'parts':part_count,'bytes':total,'shader_coverage_verified':False,
                'index_sha256':digest(index_path),'files':[dict(f,staged_file=paths[i].as_posix()) for i,f in enumerate(files)]}
        (out/'zone-validation.json').write_text(json.dumps(report,indent=2)+'\n')
        shutil.copyfile(index_path,out/'export-index.json')
        return report
    except Exception:
        shutil.rmtree(out);raise

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('index',type=Path);p.add_argument('--out',required=True,type=Path)
    a=p.parse_args();r=validate(a.index,a.out)
    print(f"Validated {r['fastfiles']} fastfiles in {r['batches']} batches; {r['bytes']} bytes; shader extraction/compilation pending")
