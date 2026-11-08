"""The firmware assembler owns platform/runtime EDK II compatibility checks."""
from pathlib import Path
import runpy
import unittest

VERIFY = runpy.run_path(str(Path(__file__).resolve().parents[1]/'build.py'))['verify_abi_sources']


class AbiSourcesTest(unittest.TestCase):
    def setUp(self):
        self.edk2 = 'a'*40
        self.libc = 'b'*40
        self.support = {'edk2': self.edk2, 'libc': self.libc}
        self.upstream = {'dependencies': {'edk2-libc': {'commit': self.libc}}}

    def test_matching_platform_and_runtime_do_not_require_runtime_default_edk2(self):
        VERIFY(self.support, self.upstream, self.edk2)

    def test_mismatched_platform_is_rejected(self):
        with self.assertRaisesRegex(SystemExit, 'W202DS platform EDK II'):
            VERIFY(self.support, self.upstream, 'c'*40)

    def test_mismatched_selected_libc_is_rejected(self):
        self.upstream['dependencies']['edk2-libc']['commit'] = 'c'*40
        with self.assertRaisesRegex(SystemExit, 'edk2-libc differs'):
            VERIFY(self.support, self.upstream, self.edk2)


if __name__ == '__main__':
    unittest.main()
