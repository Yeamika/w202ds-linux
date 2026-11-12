import runpy
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
API = runpy.run_path(str(ROOT/'tools/lk_container.py'))


class LkDebugProfileTest(unittest.TestCase):
    def test_debug_profile_requires_watchdog_usb_ramboot_and_verified_flash(self):
        features = {
            'watchdog': True,
            'usb_recovery': True,
            'sdk_oem': True,
            'fastboot_flash': True,
            'screen': False,
        }
        API['require_debug_features'](features)

    def test_release_baseline_is_rejected_for_debug_build(self):
        features = {
            'watchdog': False,
            'usb_recovery': False,
            'sdk_oem': True,
            'fastboot_flash': True,
            'screen': False,
        }
        with self.assertRaisesRegex(ValueError, 'watchdog.*usb_recovery'):
            API['require_debug_features'](features)

    def test_lcd_instrumentation_is_rejected_for_menu_owned_display(self):
        features = {
            'watchdog': True,
            'usb_recovery': True,
            'sdk_oem': True,
            'fastboot_flash': True,
            'screen': True,
        }
        with self.assertRaisesRegex(ValueError, 'screen must be disabled'):
            API['require_debug_features'](features)

    def test_debug_export_keeps_ram_tools_and_omits_lcd_probe(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            generated = root/'generated'
            generated.mkdir()
            output = root/'output'
            output.mkdir()
            baseline = root/'baseline.img'
            baseline.write_bytes(b'baseline')
            for name in ('ramboot-memory.bin', 'ramboot-smoke.bin', 'ramboot-hang.bin',
                         'ramboot-screen.bin'):
                (generated/name).write_bytes(name.encode())
            artifacts = API['export_debug_tools'](ROOT/'../lk/legacy-buildchain', generated, baseline, output)
            self.assertEqual({path.name for path in artifacts}, {
                'lk-debuger.img', 'ramboot-memory.bin', 'ramboot-smoke.bin',
                'ramboot-hang.bin', 'read.py'})
            self.assertEqual((output/'lk-debug-tools/lk-debuger.img').read_bytes(), b'baseline')
            self.assertTrue((output/'lk-debug-tools/read.py').is_file())
            self.assertFalse((output/'lk-debug-tools/ramboot-screen.bin').exists())


if __name__ == '__main__':
    unittest.main()
