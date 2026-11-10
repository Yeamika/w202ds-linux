"""Resolve the same component and board paths in local and cloned layouts."""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def default_repository(name, marker, root=ROOT):
    for candidate in (root/'components'/name, root/name, root.parent/name,
                      root.parent.parent/name):
        if (candidate/marker).is_file():
            return candidate
    return root.parent.parent/name


def lk_directory(root=ROOT):
    return root/'lk' if (root/'lk/ramboot').is_dir() else root.parent/'lk'


def edk2_directory():
    explicit = os.environ.get('PJB_EDK2_ROOT')
    edk2 = (Path(explicit) if explicit else
            default_repository('edk2-ums9620', 'build.sh')/'edk2').resolve()
    if not (edk2/'MdePkg/Include/Uefi.h').is_file():
        raise RuntimeError('Set PJB_EDK2_ROOT to an initialized upstream EDK II checkout')
    return edk2
