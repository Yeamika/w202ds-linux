#!/usr/bin/env python3
"""W202DS BROM → FDL1 → auxiliary payload → FDL2 → U-Boot."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from download import ensure_tool

ROOT = Path(__file__).resolve().parent
LOADERS = {
    'fdl1-dl.bin': '4bebf3d2f19610023cadd2c536e8c22911ba942dcfa10dc1ac2bd317e349cd07',
    'fdl2-dl.bin': '95fd4ecaeaf0d7d762c45be37771a58b2bc7a59339fbd76293c09d04378eeb61',
    'custom_exec_no_verify_65012f48.bin': '3842a80777dcde859d9d235745379128442b48ff5e290ab1e1fb9e98fa073fda',
}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tool', type=Path, help='spd_dump executable (absolute or relative path)')
    p.add_argument('--loaders', type=Path, default=ROOT / 'loaders')
    p.add_argument('--image', type=Path, help='U-Boot image; omitted for interactive FDL2 session')
    p.add_argument('--partition', choices=('uboot', 'uboot_a', 'uboot_b'), default='uboot')
    p.add_argument('--wait', type=int, default=300)
    p.add_argument('--execute', action='store_true', help='actually connect/write; default is dry-run')
    a = p.parse_args()
    if a.wait < 1:
        p.error('--wait must be positive')
    loaders = a.loaders.resolve()
    for name, expected in LOADERS.items():
        f = loaders / name
        if not f.is_file() or hashlib.sha256(f.read_bytes()).hexdigest() != expected:
            p.error('missing or incorrect loader: ' + str(f))
    image = a.image.resolve() if a.image else None
    if image and (not image.is_file() or not 0 < image.stat().st_size <= 5242880):
        p.error('--image must be a nonempty file no larger than 5 MiB')
    name = 'spd_dump.exe' if os.name == 'nt' else 'spd_dump'
    local = ROOT / 'spd_dump' / ('win' if os.name == 'nt' else 'linux') / name
    tool = a.tool.resolve() if a.tool else (local if local.is_file() else Path(shutil.which(name) or local))
    command = [str(tool), '--wait', str(a.wait), 'exec_addr', '0x65012f48',
               'fdl', 'fdl1-dl.bin', '0x65000800',
               'fdl', 'fdl2-dl.bin', '0xb4fffe00', 'exec']
    if image:
        command += ['w', a.partition, str(image), 'reset']
    print(json.dumps({'execute': a.execute, 'argv': command,
                      'auxiliary': str(loaders / 'custom_exec_no_verify_65012f48.bin'),
                      'image_sha256': hashlib.sha256(image.read_bytes()).hexdigest() if image else None}, indent=2), flush=True)
    if not a.execute:
        return 0
    if not tool.is_file() and not a.tool:
        try:
            tool = ensure_tool(ROOT / 'spd_dump')
        except Exception as error:
            p.error('tool download failed: ' + str(error))
        command[0] = str(tool)
    if not tool.is_file():
        p.error('executable not found: ' + str(tool))
    # spd_dump locates the auxiliary payload relative to cwd. Stage all loaders
    # together; preserve the caller's directory and inherit interactive stdio.
    with tempfile.TemporaryDirectory(prefix='w202ds-fdl-') as staging:
        for filename in LOADERS:
            shutil.copyfile(loaders / filename, Path(staging) / filename)
        return subprocess.run(command, cwd=staging, check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
