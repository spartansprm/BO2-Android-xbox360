#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only


import argparse
import hashlib
import importlib.util
import json
import multiprocessing
from pathlib import Path
import re
import sys
import time
import uuid
import zipfile

from validate_android_zone_batches import BASE,PATCHES,MIB,digest,need,valid_hash

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/inspect'))
sys.path.insert(0,str(ROOT/'tools/install'))
import ff_shaders as ff
import ff_shaders_fast as fast
from game_files import zone_modes

def save(path,value):
    temp=path.with_suffix(path.suffix+'.tmp');temp.write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n');temp.replace(path)

class Extractor:
    def __init__(self,out,image,library,jobs=2):
        need(digest(image)=='ecb2d7ce653be4c4fc8945e0083e222570266b5a770896819427be69d463f72e','Unverified TU18 image')
        self.out=out;self.containers=out/'containers';self.containers.mkdir(exist_ok=True)
        self.key,_=ff.key_from_xex_image(str(image));fast.initialize(library)

        for n in (0,1,63,64,65,1024,32768):
            k=hashlib.sha256(f'authored-key-{n}'.encode()).digest();v=hashlib.sha256(f'authored-nonce-{n}'.encode()).digest()[:8]
            need(fast.salsa20_keystream(k,v,n)==ff.salsa20_keystream(k,v,n),'Salsa20 accelerator/reference mismatch')
        ff.salsa20_keystream=fast.salsa20_keystream
        self.pool=multiprocessing.Pool(jobs)
        self.checked=0;self.native_chunks=0;self.fallbacks=0

    def decode(self,path,zone_id):
        info,chunks=ff.read_container(str(path));name=path.stem
        per_stream=self.pool.map(ff.decrypt_stream,[(self.key,name,s,chunks[s::4]) for s in range(4)])
        plain=[None]*len(chunks)
        for s,values in enumerate(per_stream):plain[s::4]=values
        if info['compression']=='lzx':
            parts=self.pool.map(fast.decompress_job,plain,chunksize=16)
            for i,p in enumerate(parts):
                self.native_chunks+=1
                check=self.checked<64 or (zone_id%17==0 and i in (0,len(parts)-1)) or self.native_chunks%4096==0
                if isinstance(p,Exception) or check:
                    reference=ff.decompress_job(plain[i])
                    if isinstance(reference,Exception):raise ValueError(f'XChunk {i}: {reference}')
                    if isinstance(p,Exception):parts[i]=reference;self.fallbacks+=1
                    else:need(p==reference,f'LZX accelerator/reference mismatch in zone {zone_id} chunk {i}')
                    self.checked+=1
        else:parts=self.pool.map(ff.inflate_job,plain,chunksize=16)
        for i,p in enumerate(parts):
            if isinstance(p,Exception):raise ValueError(f'XChunk {i}: {p}')
        zone=b''.join(parts);xf=ff.parse_xfile(zone)
        need(xf['size']==len(zone)-40,'XFile declared size does not match decoded zone')
        return info,zone

    def extract(self,path,meta,state):
        info,zone=self.decode(path,meta['id']);records=ff.find_shaders(zone)
        unmatched=[{k:r.get(k) for k in ('container','kind','virtual','physical','why')} for r in records if not r['ok']]
        names=set()
        for r in records:
            if not r['ok']:continue
            blob=r['blob'];need(24<=len(blob)<=65536,'Extracted container size outside bound')
            virtual=int.from_bytes(blob[4:8],'big');physical=int.from_bytes(blob[8:12],'big')
            need(virtual>=24 and physical>0 and virtual+physical==len(blob),'Extracted container lengths do not match')
            name=r['kind']+'_'+r['sha1'][:16]+'.bin';target=self.containers/name
            if target.exists():need(target.read_bytes()==blob,'Container name collision')
            else:target.write_bytes(blob)
            entry=state['containers'].setdefault(name,{'kind':r['kind'],'sha1':r['sha1'],'sha256':hashlib.sha256(blob).hexdigest(),'bytes':len(blob),'zones':[]})
            if meta['id'] not in entry['zones']:entry['zones'].append(meta['id'])
            names.add(name)
        return {'status':'extracted','decoded_bytes':len(zone),'xchunks':info['xchunks'],'compression':info['compression'],
                'auth_name':info['auth_name'],'filename_matches_auth':info['auth_name']==path.stem,
                'modes':sorted(zone_modes(path.stem.lower())),'records':len(records),'matched_records':len(records)-len(unmatched),
                'unmatched':unmatched,'containers':sorted(names)}

    def close(self):self.pool.close();self.pool.join()

class Processor:
    def __init__(self,index_path,out,extractor=None,delete_archives=False,consumer=None):
        self.index_path=index_path.resolve();self.out=out.resolve();self.extractor=extractor;self.consumer=consumer
        self.delete_archives=delete_archives
        need(not self.out.is_relative_to(ROOT),'Private output must stay outside the source tree')
        need(self.index_path.stat().st_size<=8*MIB,'Export index too large')
        self.index=json.loads(self.index_path.read_bytes());j=self.index
        need(j.get('schema')==1 and j.get('kind')=='bo2-fastfile-export' and j.get('private_game_resources') is True and j.get('complete') is True,'Not a complete private export')
        need(str(uuid.UUID(j['run_id']))==j['run_id'],'Invalid run ID')
        inv=j['inventory'];need(inv.get('scope')=='all-fastfiles-in-selected-base-update-dlc','Invalid export scope')
        checks=inv['source_checks'];need(checks.get('base_xex_sha256')==BASE and checks.get('patch_sha256') in PATCHES,'Unverified source pair')
        self.files=j['files'];self.batches=j['batches'];zones=inv['zones'];seen=set();total=0;self.paths=[]
        need(1<=len(zones)<=8192 and len(self.files)==len(zones) and 1<=len(self.batches)<=1024,'Invalid export counts')
        for i,(z,f) in enumerate(zip(zones,self.files)):
            need(type(z.get('id')) is int and z['id']==i and type(f.get('id')) is int and f['id']==i,'Noncanonical zone ID')
            need(all(f.get(k)==z.get(k) for k in ('origin','path','bytes')),'File/inventory mismatch')
            size=f['bytes'];need(type(size) is int and 1<=size<=1024*MIB and valid_hash(f.get('sha256')),'Invalid zone size/hash')
            origin=f['origin'];need(origin in ('game','update') or isinstance(origin,str) and re.fullmatch(r'dlc/[^/\\\x00]{1,42}',origin),'Invalid origin')
            p=f['path'];need(isinstance(p,str) and len(p)<=1024,'Invalid source path')
            name=p.replace('\\','/').rsplit('/',1)[-1];need(re.fullmatch(r'[A-Za-z0-9_.-]{1,180}\.ff',name,re.I),'Invalid fastfile basename')
            need((origin,p) not in seen,'Duplicate source zone');seen.add((origin,p));total+=size
            self.paths.append(self.out/'raw'/f'z{i:04d}'/name)
        need(total==j.get('total_bytes') and total<=16*1024*MIB,'Invalid total byte count')
        self.out.mkdir(parents=True,exist_ok=True);self.journal=self.out/'extraction-state.json'
        if self.journal.exists():
            self.state=json.loads(self.journal.read_text());need(self.state['index_sha256']==digest(self.index_path),'Journal belongs to another index')
        else:
            self.state={'schema':1,'status':'working','run_id':j['run_id'],'index_sha256':digest(self.index_path),
              'expected_batches':len(self.batches),'expected_files':len(self.files),'expected_bytes':total,
              'batches':[],'received':[0]*len(self.files),'files':{},'containers':{},'parts_seen':[],'validated_bytes':0}
        self.parts_seen=set(self.state['parts_seen']);self.checkpoint()

    def checkpoint(self):
        s=self.state;s['parts_seen']=sorted(self.parts_seen)
        s['summary']={'batches':len(s['batches']),'files':len(s['files']),'unique_containers':len(s['containers']),
            'failed_zones':sum(v.get('status')=='error' for v in s['files'].values()),
            'unmatched_candidates':sum(len(v.get('unmatched',[])) for v in s['files'].values()),'validated_bytes':s['validated_bytes']}
        if self.extractor:s['codec_checks']={'salsa20_reference_vectors':7,'lzx_reference_chunks':self.extractor.checked,
            'native_lzx_chunks':self.extractor.native_chunks,'reference_fallbacks':self.extractor.fallbacks}
        save(self.journal,s)

    def complete_zone(self,i):
        path=self.paths[i];meta=self.files[i]
        need(path.stat().st_size==meta['bytes'] and digest(path)==meta['sha256'],'Complete fastfile hash mismatch')
        row={k:meta[k] for k in ('id','origin','path','bytes','sha256')};row['transport_verified']=True
        if self.extractor:
            try:row.update(self.extractor.extract(path,meta,self.state))
            except Exception as e:
                if 'accelerator/reference mismatch' in str(e):raise
                row.update(status='error',error=str(e))
        else:
            if self.consumer:self.consumer(meta,path)
            row['status']='transport_verified'
        self.state['files'][str(i)]=row;path.unlink();path.parent.rmdir()
        print(f"zone {i+1}/{len(self.files)} {meta['origin']}/{meta['path']}: {row['status']}; {len(row.get('containers',[]))} unique, {len(row.get('unmatched',[]))} unmatched",flush=True)

    def ingest(self,archive):
        number=len(self.state['batches'])+1;need(number<=len(self.batches),'Unexpected extra batch')
        batch=self.batches[number-1];need(batch.get('file')==f'bo2-zonas-{number:04d}.zip','Batch order/name mismatch')
        need(archive.is_file() and not archive.is_symlink() and 1<=archive.stat().st_size<=32*MIB and
             archive.stat().st_size==batch['bytes'] and digest(archive)==batch['sha256'],'Batch size/hash mismatch')
        with zipfile.ZipFile(archive) as z:
            infos=z.infolist();names=[x.filename for x in infos]
            need(len(names)==len(set(names)) and len(infos)<=8193,'Duplicate/excess ZIP entries')
            need(all(not x.is_dir() and not x.flag_bits&1 and (x.external_attr>>16)&0o170000!=0o120000 for x in infos),'Unsupported ZIP entry')
            need(sum(x.file_size for x in infos)<=32*MIB and z.getinfo('zone_batch_manifest.json').file_size<=4*MIB,'ZIP expansion/manifest limit')
            m=json.loads(z.read('zone_batch_manifest.json'))
            need(m.get('schema')==1 and m.get('kind')=='bo2-fastfile-batch' and m.get('private_game_resources') is True and
                 m.get('run_id')==self.index['run_id'] and m.get('app_version')==self.index['app_version'] and
                 m.get('batch')==number and m.get('inventory')==self.index['inventory'],'Batch provenance mismatch')
            listed={'zone_batch_manifest.json'}
            for p in m['parts']:
                entry=p.get('entry');i=p.get('zone_id');offset=p.get('offset');size=p.get('bytes')
                need(type(i) is int and 0<=i<len(self.files) and re.fullmatch(r'zones/z\d{4}-p\d{4}\.bin',entry or ''),'Invalid part name/zone')
                need(entry.startswith(f'zones/z{i:04d}-') and entry not in listed and entry not in self.parts_seen,'Duplicate/misassigned part')
                need(type(size) is int and 1<=size<=24*MIB and type(offset) is int and offset==self.state['received'][i] and
                     size<=self.files[i]['bytes']-offset and valid_hash(p.get('sha256')) and z.getinfo(entry).file_size==size,'Part offset/size/hash metadata mismatch')
                target=self.paths[i];target.parent.mkdir(parents=True,exist_ok=True)
                need((offset==0 and not target.exists()) or (target.exists() and target.stat().st_size==offset),'Partial file differs from journal')
                h=hashlib.sha256();got=0
                with z.open(entry) as src,target.open('wb' if offset==0 else 'ab') as dst:
                    while data:=src.read(128*1024):dst.write(data);h.update(data);got+=len(data)
                need(got==size and h.hexdigest()==p['sha256'],'Part content hash mismatch')
                self.state['received'][i]+=size;self.state['validated_bytes']+=size
                self.parts_seen.add(entry);listed.add(entry)
                if self.state['received'][i]==self.files[i]['bytes']:self.complete_zone(i)
            need(set(names)==listed,'Unlisted ZIP content')
        self.state['batches'].append({'file':batch['file'],'bytes':batch['bytes'],'sha256':batch['sha256']})
        if number==len(self.batches):
            need(all(self.state['received'][i]==f['bytes'] for i,f in enumerate(self.files)) and len(self.state['files'])==len(self.files),'Missing complete files')
            self.state['transport_complete']=True
            self.state['status']='extracted' if self.extractor else 'transport_verified'
        self.checkpoint()
        if self.delete_archives:archive.unlink()
        print('BATCH COMPLETE',number,self.state['summary'],flush=True)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--index',required=True,type=Path);p.add_argument('--out',required=True,type=Path)
    p.add_argument('--xex-image',required=True,type=Path);p.add_argument('--lzx-library',required=True,type=Path);p.add_argument('--jobs',type=int,default=2)
    p.add_argument('--batch-dir',type=Path,help='Read all indexed ZIPs from this local folder instead of waiting for request files')
    p.add_argument('--delete-ingested-archives',action='store_true',help='Delete verified local input ZIP copies after consumption; never use on the only originals')
    a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
    extract=Extractor(a.out,a.xex_image,a.lzx_library,a.jobs);processor=Processor(a.index,a.out,extract,delete_archives=a.delete_ingested_archives)
    requests=a.out/'requests';requests.mkdir(exist_ok=True)
    try:
        while len(processor.state['batches'])<len(processor.batches):
            number=len(processor.state['batches'])+1;request=requests/f'{number:04d}.json'
            if a.batch_dir:
                processor.ingest(a.batch_dir/processor.batches[number-1]['file']);continue
            if not request.exists():time.sleep(0.25);continue
            data=json.loads(request.read_text());processor.ingest(Path(data['archive']));request.unlink()
        print('ALL BATCHES VALIDATED',processor.state['summary'],flush=True)
    except Exception as e:
        processor.state.update(status='fatal',error=str(e));processor.checkpoint();raise
    finally:extract.close()

if __name__=='__main__':main()
