#!/usr/bin/env python3
"""Verify W202DS firmware formats, LK partition layout and packaging hashes."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def verify(directory, manifest_name):
    directory=directory.resolve()
    record=json.loads((directory/manifest_name).read_text())
    if record.get('board') != 'w202ds' or record.get('stage') not in ('uefi', 'efi', 'all'):
        raise ValueError('not a W202DS firmware manifest')
    results={}
    for name,expected in record['artifacts'].items():
        path=(directory/name).resolve()
        if directory not in path.parents:
            raise ValueError('artifact escapes output directory')
        data=path.read_bytes()
        if len(data)!=expected['bytes'] or hashlib.sha256(data).hexdigest()!=expected['sha256']:
            raise ValueError('artifact identity mismatch: '+name)
        if path.suffix.lower()=='.efi':
            if len(data)<64 or data[:2]!=b'MZ': raise ValueError('missing DOS/PE header: '+name)
            pe=struct.unpack_from('<I',data,60)[0]
            if pe+94>len(data) or data[pe:pe+4]!=b'PE\0\0': raise ValueError('bad PE header: '+name)
            if struct.unpack_from('<H',data,pe+4)[0]!=0xaa64: raise ValueError('not AArch64: '+name)
            if struct.unpack_from('<H',data,pe+24)[0]!=0x20b: raise ValueError('not PE32+: '+name)
            if struct.unpack_from('<H',data,pe+92)[0]!=10: raise ValueError('not an EFI application: '+name)
        if path.suffix.lower()=='.fd':
            if len(data)!=0x200000 or data[40:44]!=b'_FVH': raise ValueError('invalid W202DS FV: '+name)
            size=struct.unpack_from('<H',data,48)[0]
            if size<56 or size%2 or size>len(data) or sum(struct.unpack_from('<'+'H'*(size//2),data))&0xffff:
                raise ValueError('invalid FV header checksum: '+name)
        results[name]={'bytes':len(data),'sha256':expected['sha256']}
    if not results: raise ValueError('no boot artifacts')
    if record.get('stage') in ('uefi', 'all'):
        images = record.get('partition_images', {})
        image = images.get('uboot_b', {})
        name = image.get('path')
        if (name != 'resident/uboot-b-uefi.img' or name not in results or
                image.get('bytes') != 0x500000 or image.get('format') != 'w202ds-lk-resident' or
                image.get('sha256') != results[name]['sha256'] or results[name]['bytes'] != 0x500000):
            raise ValueError('UEFI output must include a verified 5 MiB uboot_b partition image')
        report_name = 'resident/manifest.json'
        if report_name not in results:
            raise ValueError('missing LK packaging report')
        report = json.loads((directory/report_name).read_text())
        data = (directory/name).read_bytes()
        if report.get('bytes') != len(data) or report.get('sha256') != image['sha256']:
            raise ValueError('LK packaging report differs from partition image')
        if report.get('settings_storage') != 'esp-files' or report.get('persistent_logging') is not False:
            raise ValueError('LK settings must use ESP files without persistent logging')
        layout = report['layout']
        start, size = layout['RO_FLASH_OFFSET'], layout['IMAGE_BYTES']
        if start != 0x228000 or not 0 < size <= 0x200000 or start+size > len(data):
            raise ValueError('invalid LK embedded bundle bounds')
        if hashlib.sha256(data[start:start+size]).hexdigest() != report['bundle_sha256']:
            raise ValueError('LK embedded bundle differs from packaging report')
        if any(data[start+size:]):
            raise ValueError('LK unused partition tail is not zero')
    return results


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory',type=Path)
    p.add_argument('--manifest',required=True)
    a=p.parse_args()
    print(json.dumps(verify(a.directory,a.manifest),indent=2))


if __name__=='__main__': main()
