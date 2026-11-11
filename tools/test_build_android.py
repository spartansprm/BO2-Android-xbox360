#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
import contextlib
import importlib.util
import io
import json
import os
import shutil
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

spec=importlib.util.spec_from_file_location('build_android',Path(__file__).with_name('build_android.py'))
build=importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class InputTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)
        self.source=self.root/'source'
        self.source.mkdir()
        self.target=self.root/'work'

    def pair(self,stem):
        for extension in ('xex','xexp'):
            (self.source/(stem+'.'+extension)).write_bytes(b'XEX2'+(extension*10).encode())

    def test_both_programs_can_be_imported_independently(self):
        self.pair('default_mp');self.pair('default')
        mp=build.import_inputs(self.source,self.target,['default_mp'])
        cp=build.import_inputs(self.source,self.target,['default'])
        self.assertEqual(set(mp),{'default_mp.xex','default_mp.xexp'})
        self.assertEqual(set(cp),{'default.xex','default.xexp'})
        self.assertEqual(len(list(self.target.iterdir())),4)

    def test_invalid_pair_does_not_replace_existing_inputs(self):
        self.pair('default_mp')
        build.import_inputs(self.source,self.target,['default_mp'])
        before={p.name:p.read_bytes() for p in self.target.iterdir()}
        (self.source/'default_mp.xexp').write_bytes(b'invalid')
        with self.assertRaises(ValueError):build.import_inputs(self.source,self.target,['default_mp'])
        self.assertEqual(before,{p.name:p.read_bytes() for p in self.target.iterdir()})

    def make_zip(self,entries):
        path=self.root/'inputs.zip'
        with zipfile.ZipFile(path,'w') as archive:
            for name,data in entries:archive.writestr(name,data)
        return path

    def test_exported_zip_is_accepted(self):
        data=b'XEX2'+b'0'*32
        path=self.make_zip([('game/default_mp.xex',data),('game/default_mp.xexp',data),('manifest.json','{}')])
        self.assertEqual(len(build.import_inputs(path,self.target,['default_mp'])),2)

    def test_traversal_is_rejected_before_extraction(self):
        path=self.make_zip([('../default_mp.xex',b'XEX2'+b'0'*32)])
        with self.assertRaises(ValueError):build.import_inputs(path,self.target,['default_mp'])
        self.assertFalse((self.root/'default_mp.xex').exists())

    def test_duplicate_zip_entries_are_rejected(self):
        data=b'XEX2'+b'0'*32
        path=self.make_zip([('game/default_mp.xex',data),('default_mp.xex',data),('default_mp.xexp',data)])
        with self.assertRaises(ValueError):build.import_inputs(path,self.target,['default_mp'])

    def test_build_cache_archive_is_not_an_executable_input(self):
        path=self.make_zip([('work/signing/key.p12',b'not a game input')])
        with self.assertRaises(ValueError):build.import_inputs(path,self.target,['default_mp'])

    def test_source_edits_invalidate_generated_code(self):
        for path in ['campaign/codegen.sh','app/gaps.toml','sdk/src/codegen/generator.cpp']:
            p=self.root/path;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('one')
        with patch.object(build,'ROOT',self.root):
            first=build.generation_fingerprint('default_mp',{'default_mp.xex':'a'})
            (self.root/'sdk/src/codegen/generator.cpp').write_text('two')
            second=build.generation_fingerprint('default_mp',{'default_mp.xex':'a'})
            third=build.generation_fingerprint('default_mp',{'default_mp.xex':'b'})
        self.assertNotEqual(first,second)
        self.assertNotEqual(second,third)

    def test_incompatible_tu_image_is_rejected(self):
        (self.root/'default_mp.image.bin').write_bytes(b'0'*32)
        (self.root/'default_mp.summary.json').write_text(json.dumps({'title_id':'415608C3','patch_applied':True,'version':'0.0.18.14'}))
        with self.assertRaises(ValueError):build.validate_image('default_mp',self.root)


class SigningTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('keytool'),'JDK required')
    def test_local_key_is_created_once_and_kept_private(self):
        with tempfile.TemporaryDirectory() as tmp,patch.object(build,'ROOT',Path(tmp)),contextlib.redirect_stdout(io.StringIO()):
            args=type('Args',(),{'keystore':None})()
            key,alias,env=build.local_signing(dict(os.environ),args)
            data=key.read_bytes()
            other,other_alias,other_env=build.local_signing(dict(os.environ),args)
            self.assertEqual(data,other.read_bytes())
            self.assertEqual(alias,other_alias)
            self.assertEqual(env['BO2_APK_STORE_PASS'],other_env['BO2_APK_STORE_PASS'])
            self.assertEqual(key.stat().st_mode&0o777,0o600)
            self.assertEqual((key.parent/'password').stat().st_mode&0o777,0o600)

    def test_missing_password_does_not_generate_another_key(self):
        with tempfile.TemporaryDirectory() as tmp,patch.object(build,'ROOT',Path(tmp)):
            p=Path(tmp)/'work/signing';p.mkdir(parents=True);(p/'local-release.p12').write_bytes(b'keep')
            with self.assertRaises(ValueError):build.local_signing(dict(os.environ),type('Args',(),{'keystore':None})())
            self.assertEqual((p/'local-release.p12').read_bytes(),b'keep')


if __name__=='__main__':unittest.main()
