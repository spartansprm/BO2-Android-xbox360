#!/usr/bin/env python3

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zipfile
from package_android_shader_library import package_library

ROOT=Path(__file__).resolve().parents[1]


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--java-home',type=Path,required=True);ap.add_argument('--json-jar',type=Path,required=True)
    ap.add_argument('--out',type=Path,required=True);args=ap.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    temp=Path(tempfile.mkdtemp(dir=args.out));classes=temp/'classes';classes.mkdir()
    stub=temp/'android/content';(stub/'res').mkdir(parents=True)
    (stub/'Context.java').write_text('package android.content; public final class Context { private final java.io.File root; public Context(java.io.File r){root=r;} public java.io.File getFilesDir(){return root;} public android.content.res.AssetManager getAssets(){throw new AssertionError("Imported library must not be replaced with APK assets");}}')
    (stub/'res/AssetManager.java').write_text('package android.content.res; public final class AssetManager {public java.io.InputStream open(String name)throws java.io.IOException {throw new AssertionError("Unexpected asset read");}}')
    subprocess.run([args.java_home/'bin/javac','-cp',args.json_jar,'-d',classes,
                    stub/'Context.java',stub/'res/AssetManager.java',
                    ROOT/'android/app/src/main/java/org/bo2port/diagnostic/GameShaderAssets.java',
                    ROOT/'android/app/src/main/java/org/bo2port/diagnostic/ShaderLibraryInstaller.java',
                    ROOT/'tools/test_shader_library_installer.java'],check=True)
    results={}
    def invoke(name,source,reject=False):
        output=temp/(name+'-installed')
        value=subprocess.check_output([args.java_home/'bin/java','-cp',str(classes)+':'+str(args.json_jar),
            'org.bo2port.diagnostic.TestShaderLibraryInstaller',source,output,'reject' if reject else 'accept'],text=True).strip()
        results[name]=value;shutil.rmtree(output)

    def fixture(name,size,chunk):
        source=temp/(name+'-source');source.mkdir();pack=source/'bo2zm_shaders.spvpack';count=32848
        with pack.open('wb') as f:f.write(struct.pack('<8sIIQ',b'BZMSPV\0\0',1,count,0x123456789));f.truncate(size)
        index=source/'bo2zm_shaders.spvidx'
        with index.open('wb') as f:f.write(struct.pack('<8sIIQQI',b'BZMSIDX\0',1,count,size,0x123456789,0));f.truncate(80+72*count)
        report={'status':'ready','profile':'ubo-only','validated_modules':count,'unique_containers':count,
                'pack_sha256':hashlib.sha256(pack.read_bytes()).hexdigest(),'index_sha256':hashlib.sha256(index.read_bytes()).hexdigest()}
        (source/'shader-build-results.json').write_text(json.dumps(report));dst=temp/name
        package_library(source,dst,chunk_bytes=chunk);shutil.rmtree(source);return dst
    source=fixture('valid',128*1024+7,64*1024);invoke('multipart',source)
    large=fixture('boundary96',96*1024*1024+31,96*1024*1024);invoke('real_96MiB_boundary',large);shutil.rmtree(large)

    def rejected(name,change):
        dst=temp/name;shutil.copytree(source,dst);change(dst);invoke(name,dst,True);shutil.rmtree(dst)
    def edit(dst,change):
        path=dst/'biblioteca-shaders.json';j=json.loads(path.read_text());change(j);path.write_text(json.dumps(j))
    rejected('missing_zip',lambda d:(d/'bo2-biblioteca-0002.zip').unlink())
    rejected('corrupt_zip',lambda d:(d/'bo2-biblioteca-0001.zip').write_bytes(b'broken'))
    rejected('wrong_offset',lambda d:edit(d,lambda j:j['files'][0]['chunks'][1].update(offset=1)))
    rejected('wrong_complete_hash',lambda d:edit(d,lambda j:j['files'][0].update(sha256='0'*64)))
    rejected('wrong_slice_hash',lambda d:edit(d,lambda j:j['files'][0]['chunks'][0].update(sha256='0'*64)))
    rejected('wrong_zip_hash',lambda d:edit(d,lambda j:j['files'][0]['chunks'][0].update(zip_sha256='0'*64)))
    rejected('path_traversal',lambda d:edit(d,lambda j:j['files'][0].update(file='../bad')))
    rejected('duplicate_zip',lambda d:edit(d,lambda j:j['files'][0]['chunks'][1].update(zip=j['files'][0]['chunks'][0]['zip'])))
    rejected('excessive_count',lambda d:edit(d,lambda j:j.update(shaders=65537)))
    rejected('index_for_another_pack',lambda d:edit(d,lambda j:j.update(shaders=32849)))
    def added_file(d):
        path=d/'bo2-biblioteca-0001.zip'
        with zipfile.ZipFile(path,'a') as z:z.writestr('../extra',b'unlisted')
        edit(d,lambda j:j['files'][0]['chunks'][0].update(zip_bytes=path.stat().st_size,zip_sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
    rejected('extra_zip_entry',added_file)
    (args.out/'results.json').write_text(json.dumps({'invented_data_only':True,'results':results},indent=2)+'\n')
    shutil.rmtree(temp)
    print(json.dumps(results,indent=2))


if __name__=='__main__':main()
