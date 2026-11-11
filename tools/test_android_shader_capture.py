#!/usr/bin/env python3

import copy
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import warnings
import zipfile
from validate_android_shader_capture import validate_capture,merge_containers,IMAGE_SHA,MAX_LIBRARY_SHADERS

class CaptureTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.path=Path(self.temp.name)/'capture.zip'
        self.name='containers/v_'+'a'*32+'.bin'
        self.blob=struct.pack('>III',0x102A1101,24,24)+bytes(36)
        self.manifest={'schema':1,'kind':'bo2-runtime-shader-capture','private_game_resources':True,
            'run_id':'1f599c1e-c0b1-41d2-946d-edf338465449','loaded_image_sha256':IMAGE_SHA,
            'containers':[{'file':self.name,'bytes':len(self.blob),'sha256':hashlib.sha256(self.blob).hexdigest()}]}
    def tearDown(self):self.temp.cleanup()
    def test_dlc_library_above_previous_limit(self):
        blobs=[self.blob[:-4]+struct.pack('>I',i) for i in range(8193)]
        self.assertEqual(len(merge_containers(blobs)),8193)
    def test_library_budget_after_deduplication(self):
        self.assertEqual(len(merge_containers([self.blob]*(MAX_LIBRARY_SHADERS+1))),1)
        with self.assertRaisesRegex(ValueError,'budget'):
            merge_containers([self.blob]*(2*MAX_LIBRARY_SHADERS+1))
    def test_unique_library_limit(self):
        blobs=[self.blob[:-4]+struct.pack('>I',i) for i in range(MAX_LIBRARY_SHADERS+1)]
        self.assertEqual(len(merge_containers(blobs[:-1])),MAX_LIBRARY_SHADERS)
        with self.assertRaisesRegex(ValueError,'Unique'):
            merge_containers(blobs)
    def write(self,payloads=None):
        with zipfile.ZipFile(self.path,'w',zipfile.ZIP_DEFLATED) as z:
            z.writestr('capture_manifest.json',json.dumps(self.manifest))
            for name,data in (payloads or {self.name:self.blob}).items():z.writestr(name,data)
    def test_valid_exact_bytes(self):
        self.write();_,blobs=validate_capture(self.path);self.assertEqual(blobs,{self.name:self.blob})
    def test_modified_hash(self):
        self.write({self.name:self.blob[:-1]+b'x'})
        with self.assertRaisesRegex(ValueError,'SHA-256'):validate_capture(self.path)
    def test_path_traversal(self):
        for name in ['../escape.bin','/containers/v_bad.bin','containers/p_aaa.bin']:
            self.manifest['containers'][0]['file']=name;self.write({name:self.blob})
            with self.assertRaisesRegex(ValueError,'path'):validate_capture(self.path)
    def test_extra_game_data(self):
        self.write({self.name:self.blob,'default_mp.xex':b'forbidden'})
        with self.assertRaisesRegex(ValueError,'Unlisted'):validate_capture(self.path)
    def test_duplicate_manifest(self):
        self.manifest['containers']*=2;self.write()
        with self.assertRaisesRegex(ValueError,'Duplicate'):validate_capture(self.path)
    def test_duplicate_zip(self):
        self.write()
        with warnings.catch_warnings():
            warnings.simplefilter('ignore')
            with zipfile.ZipFile(self.path,'a') as z:z.writestr(self.name,self.blob)
        with self.assertRaisesRegex(ValueError,'Duplicate'):validate_capture(self.path)
    def test_bad_header_and_stage(self):
        self.blob=struct.pack('>III',0x102A1100,24,24)+bytes(36)
        self.manifest['containers'][0]['sha256']=hashlib.sha256(self.blob).hexdigest();self.write()
        with self.assertRaisesRegex(ValueError,'stage'):validate_capture(self.path)
        self.blob=b'x'*48;self.manifest['containers'][0]['sha256']=hashlib.sha256(self.blob).hexdigest();self.write()
        with self.assertRaisesRegex(ValueError,'header'):validate_capture(self.path)
    def test_size_and_image(self):
        self.manifest['containers'][0]['bytes']=65537;self.write()
        with self.assertRaisesRegex(ValueError,'budget'):validate_capture(self.path)
        self.manifest['loaded_image_sha256']='0'*64;self.write()
        with self.assertRaisesRegex(ValueError,'verification'):validate_capture(self.path)
    def test_wrong_session(self):
        session={'run_id':'other','game_entry_executed':True,'loaded_image_sha256':IMAGE_SHA}
        self.write({self.name:self.blob,'game-session.json':json.dumps(session).encode()})
        with self.assertRaisesRegex(ValueError,'Session'):validate_capture(self.path)
    def test_staged_names_match_host_packer(self):
        self.write();out=Path(self.temp.name)/'staged'
        subprocess.run([sys.executable,str(Path(__file__).with_name('validate_android_shader_capture.py')),
                        str(self.path),'--out',str(out)],check=True,capture_output=True)
        files=list((out/'containers').glob('*.bin'))
        self.assertEqual(len(files),1)
        self.assertEqual(files[0].name,'vs_'+hashlib.sha1(self.blob).hexdigest()[:16]+'.bin')
        self.assertEqual(files[0].read_bytes(),self.blob)

if __name__=='__main__':unittest.main()
