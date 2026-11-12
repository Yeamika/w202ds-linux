"""Reject incomplete or incompatible partition inputs before compiling anything."""
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
API = runpy.run_path(str(ROOT/'build-firmware.py'))


class FirmwareInputsTest(unittest.TestCase):
    def test_missing_inputs_fail_before_output_creation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            firmware = root/'kernel/Devices/touchscreen/firmware/Himax_firmware_guoxian.bin'
            firmware.parent.mkdir(parents=True)
            firmware.write_bytes(b'')
            output = root/'output'
            result = subprocess.run([sys.executable, str(ROOT/'build-firmware.py'),
                '--stage', 'uefi', '--output', str(output), '--inputs', str(root/'missing.json')],
                env=dict(os.environ, EDK_TOOLS_PATH=str(root/'tools')), capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('missing.json', result.stderr)
            self.assertFalse(output.exists())

    def test_defaults_need_no_settings_image(self):
        self.assertEqual(API['load_inputs'](None), {'lk_baseline': None})

    def test_relative_input_paths_and_wrong_board(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in ('lk.img',):
                (root/name).write_bytes(b'input')
            file = root/'inputs.json'
            record = {'schema': 1, 'board': 'w202ds', 'lk_baseline': 'lk.img'}
            file.write_text(json.dumps(record))
            self.assertEqual(API['load_inputs'](file)['lk_baseline'], root/'lk.img')
            record['board'] = 'another-board'
            file.write_text(json.dumps(record))
            with self.assertRaisesRegex(ValueError, 'board=w202ds'):
                API['load_inputs'](file)


if __name__ == '__main__':
    unittest.main()
