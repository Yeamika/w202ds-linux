#!/usr/bin/env python3
"""Wrap a W202DS EDK II FD, omitting erased suffix bytes by default."""
import argparse
import hashlib
from pathlib import Path
import struct
import uuid
import zlib

PAGE = 2048
FD_BASE = 0x82800000
FD_SIZE = 0x200000
LEGACY_FD_SIZE = 0x800000
STORAGE_LIMIT = 0x500000
SEC_GUID = uuid.UUID('3e401783-cc94-4fcd-97bc-bd35ac369d2f').bytes_le


def sec_entry(fd):
    if len(fd) not in (FD_SIZE, LEGACY_FD_SIZE) or fd[40:44] != b'_FVH':
        raise ValueError('Expected a W202DS 2 MiB FD or a legacy 8 MiB FD')
    fv_size = struct.unpack_from('<Q', fd, 32)[0]
    if fv_size not in (len(fd), len(fd) - 0x2000) or fd[fv_size:] != b'\xff' * (len(fd) - fv_size):
        raise ValueError('Invalid FV size or occupied handoff reservation')
    hdr = struct.unpack_from('<H', fd, 48)[0]
    if hdr < 56 or hdr > fv_size or hdr % 2 or sum(struct.unpack_from('<'+'H'*(hdr//2), fd)) & 0xffff:
        raise ValueError('Invalid firmware volume header')
    pos = (hdr + 7) & ~7
    while pos + 24 <= fv_size:
        length = int.from_bytes(fd[pos+20:pos+23], 'little')
        if length < 24 or length == 0xffffff or length > fv_size-pos:
            break
        if fd[pos:pos+16] == SEC_GUID and fd[pos+18] == 3:
            section = pos + 24
            while section + 4 <= pos+length:
                n = int.from_bytes(fd[section:section+3], 'little')
                if n < 4 or n == 0xffffff or n > pos+length-section:
                    break
                if fd[section+3] == 0x12:
                    te = section + 4
                    if n < 44 or fd[te:te+2] != b'VZ' or struct.unpack_from('<H', fd, te+2)[0] != 0xaa64:
                        break
                    stripped = struct.unpack_from('<H', fd, te+6)[0]
                    entry = struct.unpack_from('<I', fd, te+8)[0]
                    base = struct.unpack_from('<Q', fd, te+16)[0]
                    if stripped < 40 or te < stripped-40 or base != FD_BASE+te-(stripped-40):
                        raise ValueError('SEC image is not rebased for the declared load address')
                    offset = te + entry - (stripped-40)
                    if offset < te or offset >= te+n-4 or offset % 4:
                        raise ValueError('SEC entry leaves its TE image')
                    return offset
                section = (section+n+3) & ~3
            break
        pos = (pos+length+7) & ~7
    raise ValueError('Missing or malformed W202DS SEC entry')


def build(fd, compact=True):
    entry = sec_entry(fd)
    if compact:
        end = (len(fd.rstrip(b'\xff')) + 7) & ~7
        stored = fd[:end]
        descriptor = struct.pack('<8s8I', b'W202UEFI', 2, 40, len(fd),
                                 zlib.crc32(fd), 1, 0, len(stored), zlib.crc32(stored))
    else:
        stored = fd
        descriptor = struct.pack('<8s6I', b'W202UEFI', 1, 32, len(fd), zlib.crc32(fd), 0, 0)
    kernel = descriptor + stored
    header = bytearray(PAGE)
    struct.pack_into('<8s10I', header, 0, b'ANDROID!', len(kernel), FD_BASE, 0, 0,
                     0, 0, 0, PAGE, 0, 0)
    header[48:55] = b'W202DS\0'
    digest = hashlib.sha1(kernel + struct.pack('<III', len(kernel), 0, 0)).digest()
    header[576:596] = digest
    image = bytes(header) + kernel
    image += bytes((-len(image)) % PAGE)
    if compact and len(image) > STORAGE_LIMIT:
        raise ValueError('Stored image exceeds the 5 MiB limit after removing the erased suffix')
    return image, entry


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--fd', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    mode = p.add_mutually_exclusive_group()
    mode.add_argument('--compact', dest='compact', action='store_true', default=True,
                      help='omit the erased FD suffix (default); no compression codec')
    mode.add_argument('--full', dest='compact', action='store_false',
                      help='retain the full FD in a legacy version-1 diagnostic wrapper')
    a = p.parse_args()
    if a.output.exists():
        p.error('output already exists')
    image, entry = build(a.fd.read_bytes(), compact=a.compact)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_bytes(image)
    print(f'{a.output}: {len(image)} bytes, SEC=0x{FD_BASE+entry:08x}, sha256={hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
