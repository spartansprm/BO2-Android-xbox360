#!/usr/bin/env python3

import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
import zipfile
from build_android_game_shaders import validate_batch


class BatchTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name)/'resources.zip'
        self.name = 'zones/game/code_post_gfx_zm.ff'
        self.data = b'TAffx100'+(146).to_bytes(4, 'big')+b'PHEEBs71'+bytes(296)
        self.manifest = {'schema': 1, 'private_game_resources': True, 'native_vfs': {
            'passed': True, 'native_inputs_match_previous_preparation': True,
            'exports': [{'file': self.name, 'bytes': len(self.data),
                         'sha256': hashlib.sha256(self.data).hexdigest()}]}}

    def tearDown(self):
        self.temp.cleanup()

    def write(self, manifest=None, names=None):
        with zipfile.ZipFile(self.path, 'w', zipfile.ZIP_DEFLATED) as z:
            z.writestr('resource_manifest.json', json.dumps(manifest or self.manifest))
            for name, data in (names or {self.name: self.data}).items():
                z.writestr(name, data)

    def test_exact_manifest_and_hash(self):
        self.write()
        _, files = validate_batch(self.path)
        self.assertEqual(files, {self.name: self.data})

    def test_modified_resource(self):
        self.write(names={self.name: self.data[:-1]+b'X'})
        with self.assertRaisesRegex(ValueError, 'SHA-256'):
            validate_batch(self.path)

    def test_unverified_reads(self):
        self.manifest['native_vfs']['native_inputs_match_previous_preparation'] = False
        self.write()
        with self.assertRaisesRegex(ValueError, 'verification'):
            validate_batch(self.path)

    def test_extra_executable(self):
        self.write(names={self.name: self.data, 'default_mp.xex': b'private'})
        with self.assertRaisesRegex(ValueError, 'unlisted'):
            validate_batch(self.path)

    def test_paths_and_budget(self):
        for name in ['../escape.ff', '/zones/game/patch.ff', 'zones/game/unknown.ff']:
            manifest = copy.deepcopy(self.manifest)
            manifest['native_vfs']['exports'][0]['file'] = name
            self.write(manifest, {name: self.data})
            with self.assertRaisesRegex(ValueError, 'path'):
                validate_batch(self.path)
        self.manifest['native_vfs']['exports'][0]['bytes'] = 28*1024*1024+1
        self.write()
        with self.assertRaisesRegex(ValueError, 'budget'):
            validate_batch(self.path)

    def test_false_container_header(self):
        self.data = bytes(316)
        self.manifest['native_vfs']['exports'][0]['sha256'] = hashlib.sha256(self.data).hexdigest()
        self.write()
        with self.assertRaisesRegex(ValueError, 'header'):
            validate_batch(self.path)


if __name__ == '__main__':
    unittest.main()
