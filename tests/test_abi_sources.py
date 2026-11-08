"""The firmware assembler owns platform/runtime EDK II compatibility checks."""
from pathlib import Path
import runpy
import unittest

API = runpy.run_path(str(Path(__file__).resolve().parents[1]/'build.py'))
VERIFY = API['verify_abi_sources']


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


class PlatformSourcesTest(unittest.TestCase):
    def setUp(self):
        self.names = ('edk2', 'edk2/BaseTools/Source/C/BrotliCompress/brotli',
                      'edk2/MdeModulePkg/Library/BrotliCustomDecompressLib/brotli')
        self.record = {'dependencies': {name: {'repository': 'source/'+name, 'commit': 'a'*40,
                                             'gitlink': 'a'*40} for name in self.names}}
        self.platform = {'edk2': 'a'*40, 'upstream_sources': self.record}

    def test_options_are_routed_to_their_own_builders(self):
        platform, runtime = API['route_source_options'](
            ['edk2=branch', 'pocketjs=latest', 'brotli=tag', 'quickjs=commit'], self.names)
        self.assertEqual(platform, ['edk2=branch', 'brotli=tag'])
        self.assertEqual(runtime, ['pocketjs=latest', 'quickjs=commit'])
        with self.assertRaises(ValueError):
            API['route_source_options'](['typo=latest'], self.names)

    def test_matching_full_platform_identity_is_accepted(self):
        API['verify_platform_sources'](self.platform, self.record)

    def test_same_edk2_with_different_brotli_is_rejected(self):
        expected = {'dependencies': {name: dict(value) for name, value in self.record['dependencies'].items()}}
        expected['dependencies'][self.names[1]]['commit'] = 'b'*40
        with self.assertRaisesRegex(SystemExit, 'Platform build upstream differs'):
            API['verify_platform_sources'](self.platform, expected)

    def test_legacy_platform_only_accepts_the_original_gitlinks(self):
        platform = {'edk2': 'a'*40}
        API['verify_platform_sources'](platform, self.record)
        self.record['dependencies'][self.names[1]]['commit'] = 'b'*40
        with self.assertRaisesRegex(SystemExit, 'Legacy platform manifest'):
            API['verify_platform_sources'](platform, self.record)


if __name__ == '__main__':
    unittest.main()
