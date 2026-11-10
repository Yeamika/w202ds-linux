"""Execute the migrated EFI watchdog and touch lifecycle code on the host."""
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PATHS = runpy.run_path(str(ROOT/'tools/project_paths.py'))


class EfiHostTest(unittest.TestCase):
    def test_watchdog_and_touch_lifecycle(self):
        edk2 = PATHS['edk2_directory']()
        runtime = PATHS['default_repository']('pj4edk2', 'build.sh')
        abi = Path(os.environ.get('PJB_BOOT_ABI_INCLUDE') or runtime/'pocketjsforedk2/include')
        with tempfile.TemporaryDirectory(dir=os.environ.get('W202_TEST_OUTPUT')) as tmp:
            for name in ('linux_watchdog', 'touch_scope'):
                executable = Path(tmp)/name
                subprocess.run([
                    os.environ.get('CC', 'cc'), '-std=c11', '-ffunction-sections', '-fdata-sections',
                    '-Wl,--gc-sections', '-fshort-wchar', '-fno-builtin', '-Werror',
                    '-DEFIAPI=__attribute__((ms_abi))',
                    '-I'+str(edk2/'MdePkg/Include'), '-I'+str(edk2/'MdePkg/Include/X64'),
                    '-I'+str(abi), str(ROOT/'tests'/(name+'.c')), '-o', str(executable),
                ], check=True)
                subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    unittest.main()
