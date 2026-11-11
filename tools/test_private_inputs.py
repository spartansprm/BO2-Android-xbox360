#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only

import hashlib
import json
from pathlib import Path
import tempfile
import unittest
import warnings
import zipfile
from verify_private_inputs import unpack


class PrivateInputsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.payload = b'XEX2' + bytes(128)
        item = {'bytes': len(self.payload), 'sha256': hashlib.sha256(self.payload).hexdigest()}
        self.manifest = {'schema': 1, 'private_game_inputs': True,
                         'default_mp_xex': dict(item), 'default_mp_xexp': dict(item)}

    def archive(self, names=None):
        path = self.root / 'test.zip'
        with warnings.catch_warnings():
            warnings.simplefilter('ignore', UserWarning)
            with zipfile.ZipFile(path, 'w') as z:
                z.writestr('manifest.json', json.dumps(self.manifest))
                for name in names or ['game/default_mp.xex', 'game/default_mp.xexp']:
                    z.writestr(name, self.payload)
        return path

    def test_valid_pair_copies_every_byte(self):
        result = unpack(self.archive(), self.root / 'out')
        for extension in ('xex', 'xexp'):
            self.assertEqual((self.root / f'out/game/default_mp.{extension}').read_bytes(), self.payload)
            self.assertEqual(result[extension]['bytes'], len(self.payload))

    def test_hash_mismatch(self):
        self.manifest['default_mp_xex']['sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'manifest mismatch'):
            unpack(self.archive(), self.root / 'out')

    def test_size_mismatch(self):
        self.manifest['default_mp_xexp']['bytes'] += 1
        with self.assertRaisesRegex(ValueError, 'manifest mismatch'):
            unpack(self.archive(), self.root / 'out')

    def test_extra_path_is_not_extracted(self):
        with self.assertRaisesRegex(ValueError, 'exactly'):
            unpack(self.archive(['game/default_mp.xex', '../escape']), self.root / 'out')
        self.assertFalse((self.root / 'escape').exists())

    def test_duplicate_name(self):
        with self.assertRaisesRegex(ValueError, 'exactly'):
            unpack(self.archive(['game/default_mp.xex', 'game/default_mp.xex']), self.root / 'out')

    def test_unexpected_schema(self):
        self.manifest['schema'] = 2
        with self.assertRaisesRegex(ValueError, 'manifest'):
            unpack(self.archive(), self.root / 'out')


if __name__ == '__main__':
    unittest.main()
