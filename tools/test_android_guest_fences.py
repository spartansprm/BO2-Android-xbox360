#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
from pathlib import Path
import tempfile
import unittest
from check_android_guest_fences import check


class FenceTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_all_ordering_instructions(self):
        text = ''
        for instruction in ('sync', 'lwsync', 'eieio', 'isync'):
            text += f'// {instruction}\n#if defined(__ANDROID__)\nstd::atomic_thread_fence(std::memory_order_seq_cst);\n#endif\n'
        (self.root / 'bo2zm_recomp.0.cpp').write_text(text)
        self.assertEqual(check(self.root)['fences'], dict.fromkeys(('sync', 'lwsync', 'eieio', 'isync'), 1))

    def test_compiler_barrier_alone_rejected(self):
        (self.root / 'bo2zm_recomp.0.cpp').write_text('// lwsync\nREX_GUEST_BARRIER();\n')
        with self.assertRaisesRegex(ValueError, 'Missing Android fence'):
            check(self.root)

    def test_missing_generated_code_rejected(self):
        with self.assertRaisesRegex(ValueError, 'missing'):
            check(self.root)


if __name__ == '__main__':
    unittest.main()
