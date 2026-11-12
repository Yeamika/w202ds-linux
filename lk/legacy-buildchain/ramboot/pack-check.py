#!/usr/bin/env python3
"""Bundle an Android-wrapped EDK II image for the stock LK RAM preflight."""
import argparse
import hashlib
from pathlib import Path
import struct

IMAGE_OFFSET = 0x10000
DOWNLOAD_LIMIT = 0x00ffffff


def bundle(loader, image):
    if len(loader) < 32 or len(loader) > IMAGE_OFFSET:
        raise ValueError('preflight code does not fit before the image')
    if loader[8:12] != b'PLK1' or struct.unpack_from('<I', loader, 12)[0] != 1:
        raise ValueError('preflight code has no PLK1 v1 header')
    if struct.unpack_from('<I', loader, 4)[0] != len(loader):
        raise ValueError('preflight code length does not match its header')
    if len(image) < 2048 or image[:8] != b'ANDROID!':
        raise ValueError('expected an Android-wrapped EDK II image')
    total = IMAGE_OFFSET + len(image)
    if total > DOWNLOAD_LIMIT:
        raise ValueError('bundle exceeds the existing LK hook download limit')
    result = bytearray(total)
    result[:len(loader)] = loader
    result[IMAGE_OFFSET:] = image
    struct.pack_into('<I', result, 4, total)
    return bytes(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--loader', type=Path, required=True)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    data = bundle(args.loader.read_bytes(), args.image.read_bytes())
    with args.output.open('xb') as out:
        out.write(data)
    print(f'{args.output}: {len(data)} bytes, sha256={hashlib.sha256(data).hexdigest()}')


if __name__ == '__main__':
    main()
