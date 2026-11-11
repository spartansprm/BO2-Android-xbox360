#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only


import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import uuid
import zipfile

ROOT=Path(__file__).resolve().parents[1]
IMAGE_SHA='ecb2d7ce653be4c4fc8945e0083e222570266b5a770896819427be69d463f72e'
MAX_BYTES=32*1024*1024
MAX_LIBRARY_SHADERS=65536


def merge_containers(blobs):


    if len(blobs)>2*MAX_LIBRARY_SHADERS or sum(map(len,blobs))>512*1024*1024:
        raise ValueError('Merged container budget exceeded')
    unique={}
    for blob in blobs:
        name=validate_container(blob)+'s_'+hashlib.sha1(blob).hexdigest()[:16]+'.bin'
        if name in unique and unique[name]!=blob:
            raise ValueError('Shader filename hash collision')
        unique[name]=blob
    if len(unique)>MAX_LIBRARY_SHADERS:
        raise ValueError('Unique library shader budget exceeded')
    return unique


def validate_container(blob):
    if not 24<=len(blob)<=65536:
        raise ValueError('Shader container size outside limits')
    flags,virtual,physical=struct.unpack_from('>III',blob)
    if flags&0xFFFFFF00!=0x102A1100 or virtual<24 or not physical or virtual+physical!=len(blob):
        raise ValueError('Invalid shader container header/length')
    return 'v' if flags&1 else 'p'


def validate_capture(path):
    if path.stat().st_size>48*1024*1024:
        raise ValueError('Capture ZIP budget exceeded')
    with zipfile.ZipFile(path) as z:
        infos=z.infolist();names=[i.filename for i in infos]
        if not 2<=len(infos)<=8195 or len(names)!=len(set(names)):
            raise ValueError('Duplicate ZIP entries or invalid count')
        if any(i.is_dir() or i.flag_bits&1 or (i.external_attr>>16)&0o170000==0o120000 for i in infos):
            raise ValueError('Directories, symlinks and encrypted entries are forbidden')
        if z.getinfo('capture_manifest.json').file_size>4*1024*1024:
            raise ValueError('Manifest budget exceeded')
        manifest=json.loads(z.read('capture_manifest.json'))
        if manifest.get('schema')!=1 or manifest.get('kind')!='bo2-runtime-shader-capture' or manifest.get('private_game_resources') is not True:
            raise ValueError('Expected a private runtime shader capture')
        if manifest.get('loaded_image_sha256')!=IMAGE_SHA:
            raise ValueError('Capture image verification failed')
        run=manifest.get('run_id','')
        if str(uuid.UUID(run))!=run:
            raise ValueError('Invalid capture run ID')
        rows=manifest.get('containers')
        if not isinstance(rows,list) or not 1<=len(rows)<=8192:
            raise ValueError('Invalid shader count')
        payloads={};total=0
        for row in rows:
            name=row.get('file','');size=row.get('bytes')
            if not re.fullmatch(r'containers/[vp]_[0-9a-f]{32}\.bin',name):
                raise ValueError('Invalid shader path')
            if name in payloads:
                raise ValueError('Duplicate shader manifest row')
            if type(size) is not int or not 24<=size<=65536 or size>MAX_BYTES-total:
                raise ValueError('Shader byte budget exceeded')
            if z.getinfo(name).file_size!=size:
                raise ValueError('Shader size mismatch')
            blob=z.read(name)
            if hashlib.sha256(blob).hexdigest()!=row.get('sha256'):
                raise ValueError('Shader SHA-256 mismatch')
            if not name.startswith('containers/'+validate_container(blob)+'_'):
                raise ValueError('Shader stage mismatch')
            payloads[name]=blob;total+=size
        if set(names)-set(payloads)-{'capture_manifest.json','game-sdk.log','game-session.json'}:
            raise ValueError('Unlisted ZIP contents')
        if 'game-sdk.log' in names:
            if z.getinfo('game-sdk.log').file_size>8*1024*1024:
                raise ValueError('SDK log budget exceeded')
            z.read('game-sdk.log')
        if 'game-session.json' in names:
            if z.getinfo('game-session.json').file_size>256*1024:
                raise ValueError('Session budget exceeded')
            session=json.loads(z.read('game-session.json'))
            if session.get('run_id')!=run or session.get('game_entry_executed') is not True or session.get('loaded_image_sha256')!=IMAGE_SHA:
                raise ValueError('Session/capture verification mismatch')
        return manifest,payloads


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('private_zip',type=Path)
    p.add_argument('--out',type=Path,required=True);p.add_argument('--base-containers',type=Path)
    args=p.parse_args();manifest,payloads=validate_capture(args.private_zip)
    destination=args.out.resolve()
    if destination.is_relative_to(ROOT) or destination.exists():
        raise ValueError('Output must be NEW and outside the public source tree')
    blobs=list(payloads.values())
    if args.base_containers:
        for file in sorted(args.base_containers.glob('*.bin')):
            if file.is_symlink() or not 24<=file.stat().st_size<=65536:
                raise ValueError('Invalid base container')
            blobs.append(file.read_bytes())
    unique=merge_containers(blobs)
    destination.mkdir(parents=True);containers=destination/'containers';containers.mkdir()
    for name,blob in unique.items():(containers/name).write_bytes(blob)
    result={'private_game_resources':True,'captured':len(payloads),'merged_unique':len(unique),
            'source_zip_sha256':hashlib.sha256(args.private_zip.read_bytes()).hexdigest(),'run_id':manifest['run_id']}
    (destination/'capture-validation.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result))


if __name__=='__main__':main()
