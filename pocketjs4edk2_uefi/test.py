#!/usr/bin/env python3
"""Run existing ARM64 resident and LK-return tests against a completed build."""
import argparse
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
from tools.project_paths import lk_directory

ROOT = Path(__file__).resolve().parent


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, required=True)
    a = p.parse_args()
    build = a.build.resolve()
    packer = runpy.run_path(str(ROOT/'tools/mkramboot.py'))
    offset = packer['sec_entry']((build/'w202ds.fd').read_bytes())
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1',
               RESIDENT_IMAGE=str(build/'resident/uboot-b-uefi.img'),
               RAMBOOT_FD=str(build/'w202ds.fd'),
               RESIDENT_ELF=str(build/'lk-bridge/ramboot-resident.elf'),
               RAMBOOT_SEC_OFFSET=hex(offset), RAMBOOT_WATCHDOG_MS='30000',
               AARCH64_NM=shutil.which('aarch64-linux-gnu-nm') or 'aarch64-linux-gnu-nm',
               AARCH64_OBJDUMP=shutil.which('aarch64-linux-gnu-objdump') or 'aarch64-linux-gnu-objdump',
               LK_RETURN_ELF=str(build/'lk-bridge/ramboot-enter.elf'),
               LK_RETURN_RESIDENT_IMAGE=str(build/'resident/uboot-b-uefi.img'))
    tests = lk_directory()/'ramboot/tests'
    for name in ('test_resident.py', 'test_return.py'):
        subprocess.run([sys.executable, '-m', 'unittest', 'discover', '-s', str(tests),
                        '-p', name, '-v'], env=env, cwd=build, check=True)


if __name__ == '__main__':
    main()
