#!/usr/bin/env python3
"""Package the pinned W202DS LK bootstrap plus a compact EDK2 resident.

This tool never accesses a device. Only the B-slot 5 MiB container is produced;
the signed second copy, certificates and both existing PJB banks are retained.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

SIZE = 0x500000
PAYLOAD = 0x228000
BANKS = 0x4e0000
BASE_SHA = '0e77583a569b0bf69b5fbd51b22136681fd818b0258f1b3dd143cf41db50d060'
LK = 0xb5000000


def sha(data):
    return hashlib.sha256(data).hexdigest()


def require(ok, message):
    if not ok:
        raise ValueError(message)


def branch(source, target):
    require((target-source) % 4 == 0 and abs(target-source) < 0x8000000,
            'branch is out of range')
    return 0x94000000 | (((target-source) >> 2) & 0x3ffffff)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=['prepare', 'package'])
    p.add_argument('--baseline', type=Path, required=True)
    p.add_argument('--settings-image', type=Path, required=True)
    p.add_argument('--loader', type=Path, required=True)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--revision', default='edk2-resident',
                   help='revision of this package, independent of the embedded FD')
    p.add_argument('--preflight', action='store_true',
                   help='describe a W202_RESIDENT_PREFLIGHT loader without DXE watchdog renewal')
    a = p.parse_args()
    base = a.baseline.read_bytes()
    current = a.settings_image.read_bytes()
    require(len(base) == SIZE and sha(base) == BASE_SHA, 'incorrect LK baseline')
    require(len(current) == SIZE, 'settings source is not a complete uboot image')
    # Same signed payload and factory function bodies must underlie both.
    require(current[:512] == base[:512] and
            current[0x113a60:PAYLOAD] == base[0x113a60:PAYLOAD],
            'installed image has another signed baseline')
    for lo, hi in [(0xda88+512, 0xdf2c+512), (0x6514+512, 0x665c+512),
                   (0x6114+512, 0x63d4+512), (0xefe4, 0xf078)]:
        require(current[lo:hi] == base[lo:hi], 'native recovery body differs')
    require(not any(base[PAYLOAD:]), 'baseline payload space is occupied')
    spec = importlib.util.spec_from_file_location('bundle', Path(__file__).with_name('pack-check.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    bundle = module.bundle(a.loader.read_bytes(), a.image.read_bytes())
    require(len(bundle) <= 0x200000 and PAYLOAD+len(bundle) <= BANKS,
            'resident exceeds executable RAM block or partition capacity')
    layout = {'RO_BYTES': len(bundle), 'DATA_BYTES': 0, 'IMAGE_BYTES': len(bundle),
              'RO_FLASH_OFFSET': PAYLOAD, 'DATA_FLASH_OFFSET': PAYLOAD+len(bundle)}
    a.output.mkdir(parents=True, exist_ok=True)
    if a.action == 'prepare':
        with (a.output/'resident-layout.inc').open('x') as f:
            f.write(''.join(f'.equ {key}, 0x{value:x}\n' for key, value in layout.items()))
        with (a.output/'ramboot-resident.bundle.bin').open('xb') as f:
            f.write(bundle)
        print(json.dumps(layout))
        return
    require((a.output/'ramboot-resident.bundle.bin').read_bytes() == bundle,
            'resident changed since preparation')
    loader = (a.output/'resident-loader.bin').read_bytes()
    start = (a.output/'resident-start.bin').read_bytes()
    require(len(loader) == 432 and len(start) == 164, 'incorrect bootstrap size')
    require(loader[:8] == bytes.fromhex('fd7bbca9fd030091'), 'incorrect bootstrap ABI')
    candidate = bytearray(base)
    changes = [(0xf3fc, loader), (0xef40, start), (0xbea10, b'pjs-load\0')]
    require(base[0xbea10:0xbea19] == b'getsocid\0', 'incorrect command table')
    # Native dispatcher keeps its handler table; the filter preserves all
    # arguments and native handlers for recovery/fastboot/charging/other modes.
    dispatch = 0xb5006670
    require(struct.unpack_from('<I', base, dispatch-LK+512)[0] == 0xd63f0000,
            'incorrect native dispatch call site')
    changes.append((dispatch-LK+512, struct.pack('<I', branch(dispatch, 0xb500f31c))))
    require(struct.unpack_from('<I', base, 0x12b38+512)[0] == 0xf9002f1f,
            'incorrect fastboot length reset')
    changes.append((0x12b38+512, struct.pack('<I', 0xd503201f)))
    # Reject any existing direct caller entering the middle of replaced bodies.
    ranges = [(0xb500ed40, 0xb500ede4), (0xb500f1fc, 0xb500f3ac)]
    body = base[512:0x113a60]
    for i in range(0, len(body)-3, 4):
        ins, source = struct.unpack_from('<I', body, i)[0], LK+i
        imm, bits = None, 0
        if ins >> 26 in (5, 37):
            imm, bits = ins & 0x3ffffff, 26
        elif ins & 0xff000010 == 0x54000000 or ins & 0x7e000000 == 0x34000000:
            imm, bits = (ins >> 5) & 0x7ffff, 19
        elif ins & 0x7e000000 == 0x36000000:
            imm, bits = (ins >> 5) & 0x3fff, 14
        if imm is not None:
            target = source + (imm-(1 << bits) if imm & (1 << (bits-1)) else imm)*4
            for lo, hi in ranges:
                require(lo <= source < hi or not lo < target < hi,
                        f'exterior branch enters replaced body: {source:x}->{target:x}')
    changes.extend([(PAYLOAD, bundle), (BANKS, current[BANKS:])])
    for offset, data in changes:
        candidate[offset:offset+len(data)] = data
    require(candidate[:512] == base[:512] and
            candidate[0x113a60:PAYLOAD] == base[0x113a60:PAYLOAD] and
            candidate[BANKS:] == current[BANKS:], 'immutable data changed')
    report = {'revision': a.revision, 'status': 'candidate-not-deployed',
              'bytes': len(candidate), 'sha256': sha(candidate),
              'baseline_sha256': sha(base), 'settings_source_sha256': sha(current),
              'settings_banks_sha256': sha(candidate[BANKS:]),
              'bundle_sha256': sha(bundle), 'layout': layout,
              'patches': [{'offset': hex(o), 'bytes': len(d)} for o, d in changes],
              'normal_modes': [2, 16], 'native_fallback_modes': [4, 5, 7],
              'resident_failure': 'native fastboot; never boot B kernel on failure',
              'preflight': a.preflight,
              'watchdog': ('loader-configured timeout, deliberate return to native fastboot' if a.preflight else
                           'loader-configured timeout, renewed by flagged DXE resident while active'),
              'panel_setup_watchdog': True,
              'cold_scanout': 'native lcd_enable followed by inherited RGB565 layer validation',
              'cold_volume_input': 'preserve EIC data mask and enable EIC164 input bit',
              'efi_os_loaders': 'supplied by FD/ESP composition; consult its validation records',
              'native_uefi_storage': 'provided by embedded FD; consult its implementation and validation records'}
    with (a.output/'uboot-b-uefi.img').open('xb') as f:
        f.write(candidate)
    with (a.output/'manifest.json').open('x') as f:
        json.dump(report, f, indent=2)
        f.write('\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
