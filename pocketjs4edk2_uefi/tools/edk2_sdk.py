#!/usr/bin/env python3
"""Export and relink an EDK II SEC + compressed DXE FV using compiled components.

The producer supplies platform FFS files, UEFI ABI libraries, and board config.
The consumer supplies the independent runtime/page archive. No source compilation
or device operation occurs during final linking.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import uuid

APP_GUID = 'edb979d9-310e-4145-950e-ac1691a1010a'
FV_GUID = '9e21fd93-9c72-4c15-8c4b-e77f1db2d792'
LZMA_GUID = 'ee4e5898-3914-4259-9d6e-dc7bd79403cf'
ABI = {'machine': 'AARCH64', 'object_format': 'ELF64', 'calling_convention': 'AAPCS64',
       'reserved_registers': ['x18'], 'panic': 'abort', 'lto': False}


def sha(p):
    with Path(p).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write(p, data):
    Path(p).write_text(json.dumps(data, indent=2) + '\n')


def run(argv):
    subprocess.run([str(x) for x in argv], check=True)


def export(a):
    if a.output.exists():
        raise ValueError('Existing SDK retained; choose a new output')
    support = json.loads((a.support / 'manifest.json').read_text())
    for name, expected in support['files'].items():
        if sha(a.support / name) != expected:
            raise ValueError('ABI support changed: ' + name)
    a.output.mkdir(parents=True)
    payload = a.output / 'payload'
    payload.mkdir()
    shutil.copytree(a.support, payload / 'abi')
    (payload / 'platform').mkdir()
    (payload / 'tools').mkdir()
    for name in ('GenFw', 'GenFfs', 'GenFv', 'GenSec', 'LzmaCompress'):
        shutil.copy2(a.tools / name, payload / 'tools' / name)
    shutil.copy2(__file__, payload / 'edk2_sdk.py')
    if a.source:
        source = a.source.resolve()
        git = lambda *args: subprocess.check_output(['git', '-C', str(source), *args], text=True)
        changed = set(git('diff', '--name-only', 'HEAD').splitlines())
        changed.update(git('ls-files', '--others', '--exclude-standard').splitlines())
        files = set(git('ls-files', '--cached', '--others', '--exclude-standard').splitlines())
        write(payload / 'source-provenance.json', {
            'commit': git('rev-parse', 'HEAD').strip(), 'origin': git('remote', 'get-url', 'origin').strip(),
            'submodules': git('submodule', 'status', '--recursive').splitlines(),
            'changed_files': {n: sha(source/n) for n in sorted(changed) if (source/n).is_file()},
            'source_files': {n: sha(source/n) for n in sorted(files) if (source/n).is_file()}})
        (payload / 'platform.patch').write_text(git('diff', '--binary', 'HEAD'))
    candidates = list(a.fv.glob('*.fd'))
    if len(candidates) != 1:
        raise ValueError('Exactly one platform FD is required to preserve fixed SEC relocation')
    platform_fd = candidates[0].read_bytes()
    if len(platform_fd) != a.size or platform_fd[40:44] != b'_FVH':
        raise ValueError('Platform FD size or FV signature does not match')
    fv_bytes = struct.unpack_from('<Q', platform_fd, 32)[0]
    if not 56 <= fv_bytes <= a.size or platform_fd[fv_bytes:] != b'\xff' * (a.size - fv_bytes):
        raise ValueError('Only an erased trailing reservation after the FV is supported')
    fixed = {}
    for offset, data in ffs_entries(platform_fd):
        if data[19] & 0x04:
            guid = str(uuid.UUID(bytes_le=data[:16]))
            if guid in fixed:
                raise ValueError('Duplicate fixed firmware file GUID')
            fixed[guid] = (offset, data)
    if not fixed:
        raise ValueError('Platform FD has no fixed firmware files')
    templates, pinned_fixed = {}, []
    for fv_name in ('FVMAIN', 'FVMAIN_COMPACT'):
        template = (a.fv / (fv_name + '.inf')).read_text()
        def replace(m):
            file = Path(m.group(1).strip())
            # GenFv records absolute paths. Read from the selected FV tree
            # when a verified platform build has moved since compilation.
            if 'Ffs' in file.parts:
                index = len(file.parts) - 1 - file.parts[::-1].index('Ffs')
                file = a.fv / Path(*file.parts[index:])
            guid = str(uuid.UUID(bytes_le=file.read_bytes()[:16]))
            if guid == APP_GUID:
                return '' # Runtime is always supplied anew by the consumer.
            if guid == FV_GUID:
                return 'EFI_FILE_NAME = {work}/compressed.ffs'
            name = guid + '.ffs'
            dest = payload / 'platform' / name
            if fv_name == 'FVMAIN_COMPACT' and guid in fixed:
                offset, data = fixed[guid]
                pinned_fixed.append({'guid': guid, 'offset': offset,
                                     'sha256': hashlib.sha256(data[:23] + data[24:]).hexdigest()})
                # GenFv flips the FFS state byte for erase polarity 1. Preserve
                # the already-rebased SEC payload from the generated FD.
                restored = bytearray(data)
                restored[23] ^= 0xff
                dest.write_bytes(restored)
            else:
                if dest.exists() and sha(dest) != sha(file):
                    raise ValueError('Conflicting platform FFS GUID')
                shutil.copy2(file, dest)
            return 'EFI_FILE_NAME = {sdk}/platform/' + name
        template = re.sub(r'^EFI_FILE_NAME\s*=\s*(.+)$', replace, template, flags=re.M)
        if fv_name == 'FVMAIN':
            template += '\nEFI_FILE_NAME = {work}/application.ffs\n'
        templates[fv_name] = template
    config = json.loads(a.config.read_text())
    data = json.dumps(config, ensure_ascii=True, separators=(',', ':')).encode()
    if not 0 < len(data) <= 65536:
        raise ValueError('Board configuration size is invalid')
    config_c = payload / 'PlatformConfig.c'
    config_c.write_text('#include <stddef.h>\nstatic const unsigned char Config[] = {' +
                        ','.join(map(str, data)) + '};\nconst unsigned char *pjb_platform_config(size_t *n) { *n=sizeof(Config); return Config; }\n')
    run(['aarch64-linux-gnu-gcc', '-c', '-Os', '-ffreestanding', '-fno-builtin', '-fno-pie',
         '-fno-stack-protector', '-fno-asynchronous-unwind-tables', '-fno-unwind-tables',
         '-ffixed-x18', '-fno-lto', config_c, '-o', payload / 'PlatformConfig.o'])
    if set(fixed) != {item['guid'] for item in pinned_fixed}:
        raise ValueError('Fixed FFS in platform FD and FV template disagree')
    write(payload / 'layout.json', {'schema': 1, 'templates': templates, 'size': a.size, 'fv_bytes': fv_bytes, 'fixed_files': pinned_fixed,
                                    'alignment': a.alignment, 'dependencies': support['dependencies'],
                                    'runtime_sha256': support['runtime_sha256']})
    write(payload / 'link-sdk.json', {'schema': 1, 'board': a.board, 'abi': ABI,
          'required_tools': ['python3', 'aarch64-linux-gnu-ld', 'aarch64-linux-gnu-nm'],
          'output_name': a.board + '.fd', 'maximum_bytes': a.size,
          'link_command': ['python3', '{sdk}/edk2_sdk.py', 'link', '--sdk', '{sdk}',
                           '--archive', '{archive}', '--work', '{work}', '--output', '{output}'],
          'verify_command': ['python3', '{sdk}/edk2_sdk.py', 'verify', '--sdk', '{sdk}',
                             '--work', '{work}', '--output', '{output}']})
    files = [{'path': str(f.relative_to(a.output)), 'bytes': f.stat().st_size,
              'mode': f.stat().st_mode & 0o777, 'sha256': sha(f)}
             for f in sorted(payload.rglob('*')) if f.is_file()]
    write(a.output / 'manifest.json', {'schema': 1, 'id': a.id, 'board': a.board,
          'kind': 'edk2-link-sdk', 'sanitizers': False, 'files': files,
          'provenance': {'edk2': support['edk2'], 'libc': support['libc'],
                         'source_fv': str(a.fv), 'board_config_sha256': sha(a.config)},
          'device_validation': 'not-tested'})
    print(json.dumps({'directory': str(a.output), 'manifest_sha256': sha(a.output / 'manifest.json')}))


def pe_validate(file):
    data = file.read_bytes()
    if data[:2] != b'MZ':
        raise ValueError('Missing DOS/PE header')
    pe = struct.unpack_from('<I', data, 60)[0]
    if data[pe:pe+4] != b'PE\0\0' or struct.unpack_from('<H', data, pe+4)[0] != 0xaa64:
        raise ValueError('Not an ARM64 PE image')
    opt = pe + 24
    if struct.unpack_from('<H', data, opt)[0] != 0x20b:
        raise ValueError('Not PE32+')
    entry = struct.unpack_from('<I', data, opt+16)[0]
    count = struct.unpack_from('<H', data, pe+6)[0]
    sec = opt + struct.unpack_from('<H', data, pe+20)[0]
    for i in range(count):
        offset = sec + i*40
        length, address = struct.unpack_from('<II', data, offset+8)
        flags = struct.unpack_from('<I', data, offset+36)[0]
        if address <= entry < address+length and flags & 0x20000000:
            return
    raise ValueError('Entry point is not in executable code')


def link(a):
    layout = json.loads((a.sdk / 'layout.json').read_text())
    a.work.mkdir(parents=True, exist_ok=True)
    # ABI/board/archive identity is verified by build.py. The runtime
    # may be rebuilt without rebuilding platform modules (for example page edits).
    deps = [a.sdk / 'abi' / name for name in layout['dependencies']]
    elf = a.work / 'PocketJsBoot.elf'
    run(['aarch64-linux-gnu-ld', '--emit-relocs', '--gc-sections', '--no-undefined',
         '-u', '_ModuleEntryPoint', '-e', '_ModuleEntryPoint', '-z', 'common-page-size=0x1000',
         '-T', a.sdk / 'abi/GccBase.lds', '--defsym=PECOFF_HEADER_SIZE=0x228',
         '-Map=' + str(a.work / 'PocketJsBoot.map'), '-o', elf, '--start-group',
         *deps, a.sdk / 'PlatformConfig.o', a.archive, '--end-group'])
    symbols = subprocess.check_output(['aarch64-linux-gnu-nm', '--defined-only', str(elf)], text=True)
    for name in ('_ModuleEntryPoint', 'pjb_main', 'pjb_platform_config'):
        if not any(line.split()[-1:] == [name] for line in symbols.splitlines()):
            raise ValueError('Missing linked entry: ' + name)
    def tool(name, *args):
        run([a.sdk / 'tools' / name, *args])
    efi = a.work / 'PocketJsBoot.efi'
    tool('GenFw', '-e', 'UEFI_APPLICATION', '-o', efi, elf)
    pe_validate(efi)
    tool('GenSec', '-s', 'EFI_SECTION_PE32', '-o', a.work/'app.pe32', efi)
    tool('GenSec', '-s', 'EFI_SECTION_USER_INTERFACE', '-n', 'PocketJS Boot', '-o', a.work/'app.ui')
    tool('GenFfs', '-t', 'EFI_FV_FILETYPE_APPLICATION', '-g', APP_GUID,
         '-o', a.work/'application.ffs', '-i', a.work/'app.pe32', '-i', a.work/'app.ui')
    values = {'work': str(a.work), 'sdk': str(a.sdk)}
    for name, template in layout['templates'].items():
        (a.work / (name + '.inf')).write_text(template.format_map(values))
    tool('GenFv', '-i', a.work/'FVMAIN.inf', '-o', a.work/'FVMAIN.Fv')
    tool('GenSec', '-s', 'EFI_SECTION_FIRMWARE_VOLUME_IMAGE', '-o', a.work/'main.sec', a.work/'FVMAIN.Fv')
    tool('GenSec', '--sectionalign', str(layout['alignment']), '-o', a.work/'main.aligned', a.work/'main.sec')
    tool('LzmaCompress', '-e', '-o', a.work/'main.lzma', a.work/'main.aligned')
    tool('GenSec', '-s', 'EFI_SECTION_GUID_DEFINED', '-g', LZMA_GUID, '-r', 'PROCESSING_REQUIRED',
         '-o', a.work/'main.guided', a.work/'main.lzma')
    tool('GenFfs', '-t', 'EFI_FV_FILETYPE_FIRMWARE_VOLUME_IMAGE', '-g', FV_GUID,
         '-o', a.work/'compressed.ffs', '-i', a.work/'main.guided')
    tool('GenFv', '-i', a.work/'FVMAIN_COMPACT.inf', '-o', a.output)
    fv_bytes = layout.get('fv_bytes', layout['size'])
    data = a.output.read_bytes()
    if len(data) != fv_bytes or not 56 <= fv_bytes <= layout['size']:
        raise ValueError('Regenerated FV does not match the reserved FD layout')
    a.output.write_bytes(data + b'\xff' * (layout['size'] - fv_bytes))
    verify(a)


def ffs_entries(data):
    offset = (struct.unpack_from('<H', data, 48)[0] + 7) & ~7
    while offset + 24 <= len(data) and data[offset:offset+24] != b'\xff' * 24:
        size = int.from_bytes(data[offset+20:offset+23], 'little')
        header = 24
        if size == 0xffffff:
            size = struct.unpack_from('<Q', data, offset+24)[0]
            header = 32
        if size < header or offset+size > len(data):
            raise ValueError('Invalid FFS boundary')
        yield offset, data[offset:offset+size]
        offset = (offset + size + 7) & ~7


def ffs_files(data):
    return (file for _, file in ffs_entries(data))


def verify(a):
    layout = json.loads((a.sdk / 'layout.json').read_text())
    data = a.output.read_bytes()
    if len(data) != layout['size'] or data[40:44] != b'_FVH':
        raise ValueError('FD size/signature mismatch')
    fv_bytes = layout.get('fv_bytes', layout['size'])
    if (not 56 <= fv_bytes <= len(data) or struct.unpack_from('<Q', data, 32)[0] != fv_bytes
            or data[fv_bytes:] != b'\xff' * (len(data) - fv_bytes)):
        raise ValueError('FV length or erased trailing reservation mismatch')
    header = struct.unpack_from('<H', data, 48)[0]
    if header < 56 or header % 2 or sum(struct.unpack('<'+'H'*(header//2), data[:header])) & 0xffff:
        raise ValueError('FV header checksum mismatch')
    pe_validate(a.work / 'PocketJsBoot.efi')
    inner = (a.work/'FVMAIN.Fv').read_bytes()
    app = (a.work/'application.ffs').read_bytes()
    # GenFv inverts the state byte for erase-polarity=1. Compare the complete
    # file except this byte, walking real FFS boundaries rather than GUID strings
    # which also occur inside BDS executable code.
    def same(left, right):
        return left[:23] == right[:23] and left[24:] == right[24:]
    if sum(same(file, app) for file in ffs_files(inner)) != 1:
        raise ValueError('Final inner FV must contain the newly linked application exactly once')
    compressed = (a.work/'compressed.ffs').read_bytes()
    if sum(same(file, compressed) for file in ffs_files(data)) != 1:
        raise ValueError('Final FD does not contain the regenerated compressed FV')
    if not layout.get('fixed_files'):
        raise ValueError('SDK lacks fixed SEC relocation information')
    fixed = {str(uuid.UUID(bytes_le=file[:16])): (offset, file)
             for offset, file in ffs_entries(data) if file[19] & 0x04}
    for expected in layout['fixed_files']:
        offset, file = fixed.pop(expected['guid'])
        if offset != expected['offset'] or hashlib.sha256(file[:23] + file[24:]).hexdigest() != expected['sha256']:
            raise ValueError('A fixed executable moved or lost its rebased contents')
    if fixed:
        raise ValueError('Unexpected fixed firmware file')
    # Verify the compressed section round-trip as well as the uncompressed staging FV.
    decoded = a.work/'roundtrip.sec'
    run([a.sdk/'tools/LzmaCompress', '-d', '-o', decoded, a.work/'main.lzma'])
    if decoded.read_bytes() != (a.work/'main.aligned').read_bytes():
        raise ValueError('Compressed FV round-trip mismatch')
    print(json.dumps({'file': str(a.output), 'bytes': len(data), 'sha256': sha(a.output),
                      'offline_validation': 'passed', 'device_validation': 'not-tested'}))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='action', required=True)
    e = sub.add_parser('export')
    for n in ('fv', 'support', 'config', 'tools', 'output'): e.add_argument('--'+n, type=Path, required=True)
    for n in ('board', 'id'): e.add_argument('--'+n, required=True)
    e.add_argument('--size', type=lambda x: int(x, 0), required=True)
    e.add_argument('--alignment', type=int, choices=(8, 16), default=8)
    e.add_argument('--source', type=Path, help='platform Git checkout for provenance')
    for action in ('link', 'verify'):
        c = sub.add_parser(action)
        for n in ('sdk', 'work', 'output'): c.add_argument('--'+n, type=Path, required=True)
        if action == 'link': c.add_argument('--archive', type=Path, required=True)
    a = p.parse_args()
    globals()[a.action](a)


if __name__ == '__main__':
    main()
