#!/usr/bin/env python3
"""W202DS BROM → FDL1 → auxiliary payload → FDL2 → partition read-back."""
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
FULL_SIZE = '0xffffffff'


def number(p, label, value):
    try:
        parsed = int(value, 0)
    except ValueError:
        p.error(label + ' must be an integer such as 0x500000')
    if parsed < 0:
        p.error(label + ' must not be negative')
    return parsed


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tool', type=Path, help='spd_dump executable (absolute or relative path)')
    p.add_argument('--loaders', type=Path, default=ROOT / 'loaders')
    mode = p.add_mutually_exclusive_group(required=True)
    mode.add_argument('--list', action='store_true', help='print the device partition table')
    mode.add_argument('--partition', help='partition name or id to read back')
    p.add_argument('--offset', default='0', help='byte offset inside the partition, default 0')
    p.add_argument('--size', help='bytes to read; omitted reads the whole partition')
    p.add_argument('--out', type=Path, help='destination file, default <partition>.bin in the current directory')
    p.add_argument('--force', action='store_true', help='overwrite an existing destination file')
    p.add_argument('--no-reset', action='store_true', help='leave the device in FDL2 instead of resetting')
    p.add_argument('--wait', type=int, default=300)
    p.add_argument('--execute', action='store_true', help='actually connect/read; default is dry-run')
    a = p.parse_args()
    if a.wait < 1:
        p.error('--wait must be positive')
    if a.list and (a.size or a.offset != '0' or a.out):
        p.error('--list takes no --offset, --size or --out')
    number(p, '--offset', a.offset)
    if a.size:
        if number(p, '--size', a.size) == 0:
            p.error('--size must be positive; omit it to read the whole partition')
    loaders = a.loaders.resolve()
    for name, expected in LOADERS.items():
        f = loaders / name
        if not f.is_file() or hashlib.sha256(f.read_bytes()).hexdigest() != expected:
            p.error('missing or incorrect loader: ' + str(f))
    out = (a.out if a.out else Path((a.partition or 'partition') + '.bin')).resolve()
    if a.partition and out.exists() and not a.force:
        p.error('destination exists, pass --force to overwrite: ' + str(out))
    name = 'spd_dump.exe' if os.name == 'nt' else 'spd_dump'
    local = ROOT / 'spd_dump' / ('win' if os.name == 'nt' else 'linux') / name
    tool = a.tool.resolve() if a.tool else (local if local.is_file() else Path(shutil.which(name) or local))
    commands = ['p'] if a.list else ['path', str(out.parent), 'read_part', a.partition,
                                    a.offset, a.size or FULL_SIZE, out.name]
    if not a.no_reset:
        commands.append('reset')
    command = [str(tool), '--wait', str(a.wait), 'exec_addr', '0x65012f48',
               'fdl', 'fdl1-dl.bin', '0x65000800',
               'fdl', 'fdl2-dl.bin', '0xb4fffe00', 'exec'] + commands
    report = {'execute': a.execute, 'argv': command,
              'auxiliary': str(loaders / 'custom_exec_no_verify_65012f48.bin')}
    if a.partition:
        report.update({'partition': a.partition, 'offset': a.offset,
                       'size': a.size or 'partition size', 'output': str(out)})
    print(json.dumps(report, indent=2), flush=True)
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
    if a.partition:
        out.parent.mkdir(parents=True, exist_ok=True)
        if out.exists():
            out.unlink()
    # spd_dump locates the auxiliary payload relative to cwd. Stage all loaders
    # together; preserve the caller's directory and inherit interactive stdio.
    with tempfile.TemporaryDirectory(prefix='w202ds-fdl-') as staging:
        for filename in LOADERS:
            shutil.copyfile(loaders / filename, Path(staging) / filename)
        code = subprocess.run(command, cwd=staging, check=False).returncode
    if a.partition and out.is_file():
        print(json.dumps({'output': str(out), 'bytes': out.stat().st_size,
                          'sha256': hashlib.sha256(out.read_bytes()).hexdigest()}, indent=2), flush=True)
    elif a.partition:
        print(json.dumps({'output': str(out), 'bytes': 0, 'written': False}), flush=True)
    return code


if __name__ == '__main__':
    raise SystemExit(main())
