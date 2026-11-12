#!/usr/bin/env python3
"""Rebuild PJB RAMBoot from the unlock tool package's pinned uboot-mod.bin."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[2]
BASE_SHA = '9bfdec9dd4b053eb676e0fc003099bb42b76090d22cdac9f860bfb578b887d99'
MOD_SHA = 'a5686ba136bb845536aa2ef811b9c643a1bdac231ee838f36d159979c530c8b3'
PROBE_SHA = 'a9e5c7c3f6cc2bdbfec359491f1691314dc91bd71781a30610a82ec50f812b92'
HOOK_SHA = '7a0b106574f4c0c6fcb78a0eecc4fa0dc8756de8753ae4020f0fa20963ddd5a7'
FINAL_SHA = 'fe2c7b3f9384453aa9fe68c3646efd1705a221fe17a0e0d4005dd2fa89051a42'

def sha(data):
    return hashlib.sha256(data).hexdigest()

def require(condition, message):
    if not condition:
        raise ValueError(message)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--base', type=Path, default=REPO/'lk/firmware/uboot-mod.bin',
                   help='original uboot-mod.bin extracted from the v6 unlock tool archive')
    p.add_argument('--cross-compile', default='aarch64-linux-gnu-')
    a = p.parse_args()
    out = a.output.resolve()
    require(out != REPO and REPO not in out.parents, 'output must be outside repository')
    require(not out.exists(), 'choose a fresh output directory')
    source = a.base.read_bytes()
    require(len(source) == 2258808 and sha(source) == MOD_SHA, 'incorrect tool-package uboot-mod.bin')
    base = source.ljust(0x500000, b'\0')
    require(sha(base) == BASE_SHA, 'zero-padded tool image differs from the historical B-slot backup')
    # Reproduce only the historical log marker; preserve the signed copy.
    old, new = b'lk consume time: %dms\n', b'PJS-LK01   time: %dms\n'
    offset = base.find(old, 512, 0x113a60)
    require(offset == 0xc4880, 'incorrect log marker location')
    probe = base[:offset]+new+base[offset+len(old):]
    require(sha(probe) == PROBE_SHA, 'log-probe hash differs')
    require(base[0xe3d0:0xe3d8] == bytes.fromhex('fd7bb6a9810500b0') and
            base[0xbe9e0:0xbe9e8] == b'getdump\0', 'incorrect hook patch sites')
    out.mkdir(parents=True)
    # A is the Android image; B is the identical input for UEFI composition.
    (out/'uboot_a.img').write_bytes(base)
    (out/'uboot_b.img').write_bytes(base)
    commands = [
        [a.cross_compile+'gcc', '-march=armv8-a', '-c', str(ROOT/'hook.S'), '-o', str(out/'hook.o')],
        [a.cross_compile+'ld', '-Ttext=0xb500e1d0', '--entry=hook',
         '--defsym=lk_icache_sync=0xb5004dc0', '--defsym=lk_status=0xb500e11c',
         '--defsym=okay_page=0xb50bc000', '--defsym=ptable_page=0xb5f01000',
         str(out/'hook.o'), '-o', str(out/'hook.elf')],
        [a.cross_compile+'objcopy', '-O', 'binary', '-j', '.text', str(out/'hook.elf'), str(out/'hook.bin')],
    ]
    for command in commands:
        subprocess.run(command, check=True)
    hook = (out/'hook.bin').read_bytes()
    require(len(hook) == 236 and sha(hook) == HOOK_SHA, 'rebuilt hook differs from historical binary')
    final = bytearray(probe)
    final[0xe3d0:0xe4bc] = hook
    # jump-rmk needs nine bytes including NUL. Relocate the adjacent OEM
    # name into four padding bytes after flashing, and retarget its ADD.
    require(base[0xbe9e0:0xbe9f0] == b'getdump\0oem\0\0\0\0\0' and
            base[0xbe9fc:0xbea00] == bytes(4) and
            base[0x12ec0:0x12ec4] == bytes.fromhex('00a01f91'),
            'incorrect command-name layout or OEM registration')
    final[0xbe9e0:0xbe9f0] = b'jump-rmk\0'.ljust(16, b'\0')
    final[0xbe9fc:0xbea00] = b'oem\0'
    final[0x12ec0:0x12ec4] = (0x91000000 | (0x7fc << 10)).to_bytes(4, 'little')
    require(final[:512] == base[:512] and final[0x113a60:] == base[0x113a60:],
            'header or signed/certificate region changed')
    require(sha(final) == FINAL_SHA, 'rebuilt RAMBoot baseline hash differs')
    (out/'uboot-b-log-probe-v1.img').write_bytes(probe)
    (out/'lk-jump-rmk.img').write_bytes(final)
    report = {'schema': 3, 'input_path': str(a.base.resolve()),
              'input_kind': 'v6-tool-package-uboot-mod', 'input_bytes': len(source),
              'input_sha256': sha(source), 'padding': 'zero-fill to 5242880 bytes',
              'base_sha256': sha(base),
              'slot_images': {name: {'bytes': len(base), 'sha256': sha(base)}
                              for name in ('uboot_a.img', 'uboot_b.img')}, 'log_probe_sha256': sha(probe),
              'hook_sha256': sha(hook), 'candidate_sha256': sha(final), 'bytes': len(final),
              'commands': commands,
              'source_sha256': sha((ROOT/'hook.S').read_bytes()), 'hardware_tested': False}
    (out/'manifest.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
