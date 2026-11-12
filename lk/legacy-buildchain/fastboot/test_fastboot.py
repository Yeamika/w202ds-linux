"""Host-test the production polled protocol with USB/storage fault injection."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

class FastbootTest(unittest.TestCase):
    def test_protocol(self):
        include = Path(os.environ['W202_HANDOFF_INCLUDE'])
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory)/'fastboot-test'
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                '-Werror', '-I'+str(include),
                str(Path(__file__).with_name('test_service.c')), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

if __name__ == '__main__':
    unittest.main()
