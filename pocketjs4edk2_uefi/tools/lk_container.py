"""Shared LK partition packaging for standalone and distribution builds."""
import atexit
import tempfile
import subprocess
import hashlib
import json
import shutil
from pathlib import Path
import runpy
import sys


def validate_inputs(lk, baseline, lock):
    expected = lock['lk_baseline']
    data = baseline.read_bytes()
    layout = runpy.run_path(str(lk/'debug/image.py'))
    if expected['bytes'] != layout['SIZE'] or expected['sha256'] != layout['LEGACY_SHA']:
        raise ValueError('LK input lock differs from the pinned RAMLK reference')
    packer = runpy.run_path(str(lk/'ramboot/package-resident.py'))
    return packer['validate_sources'](data)


def require_debug_features(features):
    required = ('watchdog', 'usb_recovery', 'sdk_oem', 'fastboot_flash')
    missing = [name for name in required if not features.get(name)]
    if features.get('screen'):
        missing.append('screen must be disabled')
    if missing:
        raise ValueError('LK debug build requires '+', '.join(missing)+
                         '; provide a matching debug baseline or let the builder generate one')


def export_debug_tools(lk, build_output, baseline, output):
    target = output/'lk-debug-tools'
    target.mkdir()
    shutil.copy2(baseline, target/'lk-debuger.img')
    names = ('ramboot-memory.bin', 'ramboot-smoke.bin', 'ramboot-hang.bin')
    for name in names:
        source = build_output/name
        if not source.is_file():
            raise ValueError('LK debug build is missing '+name)
        shutil.copy2(source, target/name)
    reader = lk/'debug/read.py'
    if not reader.is_file():
        raise ValueError('LK RAM reader source is missing')
    shutil.copy2(reader, target/'read.py')
    return [target/'lk-debuger.img', *(target/name for name in names), target/'read.py']


def package(run, output, lk, platform, baseline, revision, *, fastboot=True):
    bridge = output/'lk-bridge'
    source = lk/'ramboot'
    run('lk-bridge', 'make', '-C', source, f'OUT={bridge}', 'LK_RETURN=1',
        f'UEFI_FASTBOOT={int(fastboot)}',
        f'HANDOFF_INCLUDE={platform/"Ums9620Pkg/W202ds/Include"}', 'resident', 'enter-sec')
    run('ramboot-bundle', sys.executable, source/'pack-check.py',
        '--loader', bridge/'ramboot-enter.bin', '--image', output/'ramboot.img',
        '--output', output/'ramboot-uefi.bin')
    resident = output/'resident'
    args = ['--baseline', baseline,
            '--loader', bridge/'ramboot-resident.bin', '--image', output/'ramboot.img',
            '--output', resident, '--revision', revision]
    run('prepare-resident', sys.executable, source/'package-resident.py', 'prepare', *args)
    run('lk-bootstrap', 'make', '-C', source, f'OUT={resident}', 'resident-bootstrap')
    run('package-resident', sys.executable, source/'package-resident.py', 'package', *args)
    image = resident/'uboot-b-uefi.img'
    report = json.loads((resident/'manifest.json').read_text())
    data = image.read_bytes()
    if len(data) != 0x500000 or hashlib.sha256(data).hexdigest() != report['sha256']:
        raise ValueError('LK partition image differs from its packaging report')
    if hashlib.sha256(baseline.read_bytes()).hexdigest() != report['baseline_sha256']:
        raise ValueError('LK packaging input changed during build')
    return {'uboot_b': {'path': str(image.relative_to(output)), 'bytes': len(data),
                        'sha256': report['sha256'], 'format': 'w202ds-lk-resident',
                        'lk_base_features': report['lk_base_features'],
                        'hardware_tested': False}}


def build_ramlk(*, debug=False):
    """Rebuild disposable RAMLK input; retain it for this process lifetime."""
    temporary = tempfile.TemporaryDirectory(prefix='w202ds-ramlk-')
    atexit.register(temporary.cleanup)
    output = Path(temporary.name)/'ramlk'
    script = Path(__file__).resolve().parents[2]/'lk/legacy-buildchain/build.py'
    subprocess.run([sys.executable, str(script), '--output', str(output),
                    '--debug' if debug else '--no-debug', '--no-screen'], check=True)
    return {'image': output/'lk-debuger.img', 'output': output}
