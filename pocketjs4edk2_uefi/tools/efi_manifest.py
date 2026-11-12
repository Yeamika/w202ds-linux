"""Record the standalone EFI sources, shared ABI and actual output hashes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('output', 'source', 'platform', 'abi-include', 'edk2'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    record = {
        'schema': 1,
        'source_files': {str(f.relative_to(args.source)): sha(f)
                         for f in sorted(args.source.rglob('*')) if f.is_file()},
        'platform_lk_return_sha256': sha(args.platform/'Ums9620Pkg/W202ds/Include/LkReturn.h'),
        'platform_memory_sha256': sha(args.platform/'Ums9620Pkg/W202ds/Include/W202dsMemory.h'),
        'linux_middleware_headers': {str(f.relative_to(args.source/'LinuxBoot/common')): sha(f)
            for f in sorted((args.source/'LinuxBoot/common/include').glob('*.h'))},
        'abi_header_sha256': sha(args.abi_include/'pocketjs_boot_context.h'),
        'edk2': subprocess.check_output(['git', '-C', str(args.edk2), 'rev-parse', 'HEAD'], text=True).strip(),
        'defines': {name: os.environ.get(name, 'FALSE') for name in
                    ('W202_ANDROID_BOOT_ENTER', 'W202_LK_RETURN_PROBE_ONLY')},
        'files': {str(f.relative_to(args.output)): {'bytes': f.stat().st_size, 'sha256': sha(f)}
                  for f in sorted((args.output/'bin').glob('*.efi'))},
    }
    (args.output/'efi-build.json').write_text(json.dumps(record, indent=2)+'\n')


if __name__ == '__main__':
    main()
