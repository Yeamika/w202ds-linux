#!/usr/bin/env python3
"""Read DDR through an installed RAMBoot LK and Android SDK Fastboot."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import zlib


def decode_packets(text, address, size):
    begin = re.findall(r'PJ4EDK2 BEGIN ([0-9a-f]{8}) ([0-9a-f]{8})', text)
    if begin != [(f'{address:08x}', f'{size:08x}')]:
        raise ValueError('missing or mismatched RAM read header')
    rows = re.findall(r'PJ4EDK2 MEM ([0-9a-f]{8}) ([0-9a-f]{32})', text)
    if len(rows) != size // 16 or any(int(row[0], 16) != address + i * 16
                                     for i, row in enumerate(rows)):
        raise ValueError('missing, duplicated or out-of-order RAM data')
    data = b''.join(bytes.fromhex(row[1]) for row in rows)
    checksums = re.findall(r'PJ4EDK2 CRC ([0-9a-f]{8})', text)
    if checksums != [f'{zlib.crc32(data):08x}']:
        raise ValueError('RAM read checksum mismatch')
    return data


def decode_trace(data, address):
    offset = 0x82fff000 - address
    if offset < 0 or offset + 4096 > len(data):
        return None, b''
    trace = data[offset:offset + 4096]
    fields = struct.unpack_from('<8Q', trace)
    if fields[0] != 0x57323032484e4431:
        return {'valid': False, 'magic': hex(fields[0])}, b''
    total = fields[7]
    if total > 0xffffffff:
        return {'valid': False, 'error': 'invalid RAM trace length'}, b''
    log = trace[64:]
    text = log[:total] if total < len(log) else log[total % len(log):] + log[:total % len(log)]
    stages = {0x10: 'fd-copied', 0x11: 'before-lk-context-save',
              0x12: 'before-native-cleanup', 0x13: 'lk-context-save-rejected',
              0x22: 'native-cleanup-returned', 0x30: 'sec-entry',
              0x61: 'lk-return-entry', 0x62: 'lk-context-restored'}
    return {'valid': True, 'stage': hex(fields[1]),
            'stage_name': stages.get(fields[1], 'platform-stage'),
            'current_el': fields[2], 'sctlr_el2': hex(fields[3]),
            'daif': hex(fields[4]), 'sp': hex(fields[5]),
            'sec_entry': hex(fields[6]), 'total_log_bytes': total}, text


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--fastboot', default='fastboot')
    p.add_argument('--serial', required=True)
    p.add_argument('--module', type=Path, required=True, help='built ramboot-memory.bin')
    p.add_argument('--address', type=lambda x: int(x, 0), default=0x82ffe000)
    p.add_argument('--size', type=lambda x: int(x, 0), default=0x2000)
    p.add_argument('--output', type=Path, required=True, help='fresh output directory')
    a = p.parse_args()
    end = (0xc0000000 if 0x80000000 <= a.address < 0xc0000000 else
           0xcc800000 if 0xc8000000 <= a.address < 0xcc800000 else 0)
    if (not end or not 0 < a.size <= 0x10000
            or a.address % 16 or a.size % 16 or a.size > end - a.address):
        p.error('choose aligned DDR in 0x80000000..0xc0000000 or the UEFI bank '
                '0xc8000000..0xcc800000, up to 64 KiB')
    if a.output.exists():
        p.error('choose a fresh output directory')
    module = bytearray(a.module.read_bytes())
    if (len(module) < 32 or module[8:16] != b'PLK1\x01\0\0\0'
            or struct.unpack_from('<I', module, 4)[0] != len(module)):
        p.error('incorrect PLK1 memory reader module')
    struct.pack_into('<QQ', module, 16, a.address, a.size)
    a.output.mkdir(parents=True)
    patched = a.output/'memory-reader.bin'
    patched.write_bytes(module)
    transcript = ''
    for args in (['stage', str(patched.resolve())], ['oem', 'jump-rmk', ':probe']):
        r = subprocess.run([a.fastboot, '-s', a.serial, *args], capture_output=True,
                           text=True, errors='replace', timeout=45)
        transcript += r.stdout + r.stderr
        (a.output/'fastboot.log').write_text(transcript, encoding='utf-8')
        if r.returncode:
            raise RuntimeError('Fastboot RAM read failed; see fastboot.log')
    data = decode_packets(transcript, a.address, a.size)
    (a.output/'memory.bin').write_bytes(data)
    trace, log = decode_trace(data, a.address)
    report = {'address': hex(a.address), 'bytes': len(data),
              'sha256': hashlib.sha256(data).hexdigest(), 'trace': trace,
              'storage_writes': False}
    if log:
        (a.output/'trace.txt').write_text(log.decode('utf-8', errors='replace'), encoding='utf-8')
    (a.output/'metadata.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
